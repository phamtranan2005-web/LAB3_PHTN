#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>

// Thông tin WiFi
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// Thông tin MQTT Broker
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);
Adafruit_MPU6050 mpu;

// Khai báo chân phần cứng
const int ledPin = 25;
const int buzzerPin = 26;

// --- Định nghĩa cấu trúc dữ liệu cảm biến truyền qua Queue ---
struct SensorData {
    float ax;
    float ay;
    float az;
    float totalAcc;
    float pitch;
    float roll;
    bool isFall;
};

// --- Khai báo các QueueHandle_t ---
QueueHandle_t dataQueue;      // Hàng đợi chứa dữ liệu gửi lên MQTT
QueueHandle_t dataAlertQueue; // Hàng đợi chứa tín hiệu cảnh báo ngã

// --- Khai báo nguyên mẫu các Task ---
void vTaskTransmitData(void *pvParameters);
void vTaskReadSensor(void *pvParameters);
void vTaskAlert(void *pvParameters);

void setup_wifi() {
    delay(10);
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(ssid);

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
}

void callback(char* topic, byte* payload, unsigned int length) {
    String message = "";
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");
    Serial.println(message);

    // Điều khiển LED thủ công từ Web
    if (String(topic) == "esp32/fall/control/led") {
        if (message == "ON") {
            digitalWrite(ledPin, HIGH);
        } else if (message == "OFF") {
            digitalWrite(ledPin, LOW);
        }
    }

    // Điều khiển Buzzer thủ công từ Web
    if (String(topic) == "esp32/fall/control/buzzer") {
        if (message == "ON") {
            digitalWrite(buzzerPin, HIGH);
        } else if (message == "OFF") {
            digitalWrite(buzzerPin, LOW);
        }
    }
}

void setup() {
    Serial.begin(115200);
    
    pinMode(ledPin, OUTPUT);
    pinMode(buzzerPin, OUTPUT);
    digitalWrite(ledPin, LOW);
    digitalWrite(buzzerPin, LOW);

    // Khởi tạo cảm biến MPU6050
    if (!mpu.begin()) {
        Serial.println("Không tìm thấy chip MPU6050!");
        while (1) {
            delay(10);
        }
    }
    Serial.println("MPU6050 Found!");

    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

    // Tạo các Queue (Sức chứa tối đa 10 phần tử)
    dataQueue = xQueueCreate(10, sizeof(SensorData));
    dataAlertQueue = xQueueCreate(5, sizeof(bool));

    // --- XÂY DỰNG HỆ THỐNG RTOS (GÁN TASK VÀO CÁC CORE) ---
    
    // 1. Task truyền data: Kết nối Wi-Fi & MQTT chạy trên Core 0
    xTaskCreatePinnedToCore(
        vTaskTransmitData,   /* Hàm task */
        "TransmitDataTask",  /* Tên task */
        4096,                /* Kích thước Stack */
        NULL,                /* Tham số */
        1,                   /* Độ ưu tiên */
        NULL,                /* Task Handle */
        0                    /* Gán vào Core 0 */
    );

    // 2. Task đọc data: Đọc MPU6050 chạy trên Core 1
    xTaskCreatePinnedToCore(
        vTaskReadSensor,
        "ReadSensorTask",
        4096,
        NULL,
        2,                   /* Độ ưu tiên cao hơn */
        NULL,
        1                    /* Gán vào Core 1 */
    );

    // 3. Task cảnh báo: Điều khiển Còi/Đèn chạy trên Core 1
    xTaskCreatePinnedToCore(
        vTaskAlert,
        "AlertTask",
        2048,
        NULL,
        2,
        NULL,
        1                    /* Gán vào Core 1 */
    );
}

void loop() {
    // Để trống hoàn toàn vì FreeRTOS đã điều phối toàn bộ các Task độc lập ở dưới.
}

// ==================== CHI TIẾT CÁC TASK RTOS ====================

// 1. Task truyền data (Chạy trên Core 0)
void vTaskTransmitData(void *pvParameters) {
    setup_wifi();
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);

    SensorData receivedData;

    while (1) {
        if (!client.connected()) {
            Serial.print("Attempting MQTT connection...");
            String clientId = "ESP32Client-FallDetection-";
            clientId += String(random(0xffff), HEX);
            
            if (client.connect(clientId.c_str())) {
                Serial.println("connected");
                client.subscribe("esp32/fall/control/led");
                client.subscribe("esp32/fall/control/buzzer");
            } else {
                Serial.print("failed, rc=");
                Serial.print(client.state());
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }
        client.loop();

        // Chờ nhận dữ liệu từ hàng đợi dataQueue để gửi lên MQTT
        if (xQueueReceive(dataQueue, &receivedData, pdMS_TO_TICKS(100)) == pdPASS) {
            // Tạo chuỗi JSON gửi lên MQTT Broker
            String payload = "{";
            payload += "\"ax\":" + String(receivedData.ax, 2) + ",";
            payload += "\"ay\":" + String(receivedData.ay, 2) + ",";
            payload += "\"az\":" + String(receivedData.az, 2) + ",";
            payload += "\"totalAcc\":" + String(receivedData.totalAcc, 2) + ",";
            payload += "\"pitch\":" + String(receivedData.pitch, 2) + ",";
            payload += "\"roll\":" + String(receivedData.roll, 2) + ",";
            payload += "\"fall\":";
            payload += (receivedData.isFall ? "true" : "false");
            payload += "}";

            client.publish("esp32/fall/sensor/data", payload.c_str());
            Serial.println("Published via FreeRTOS Task (Core 0): " + payload);
        }

        vTaskDelay(pdMS_TO_TICKS(400)); // Nghỉ ngắn giữa các chu kỳ gửi mạng
    }
}

// 2. Task đọc data (Chạy trên Core 1)
void vTaskReadSensor(void *pvParameters) {
    SensorData currentData;

    while (1) {
        // Lấy dữ liệu cảm biến từ thư viện Adafruit
        sensors_event_t a, g, temp;
        mpu.getEvent(&a, &g, &temp);

        currentData.ax = a.acceleration.x;
        currentData.ay = a.acceleration.y;
        currentData.az = a.acceleration.z;

        // Tính tổng độ lớn gia tốc
        currentData.totalAcc = sqrt(currentData.ax * currentData.ax + currentData.ay * currentData.ay + currentData.az * currentData.az);

        // Tính góc nghiêng Pitch và Roll (độ)
        currentData.pitch = atan2(currentData.ax, sqrt(currentData.ay * currentData.ay + currentData.az * currentData.az)) * 180.0 / PI;
        currentData.roll = atan2(currentData.ay, sqrt(currentData.ax * currentData.ax + currentData.az * currentData.az)) * 180.0 / PI;

        // Thuật toán phát hiện té ngã
        currentData.isFall = (currentData.totalAcc > 28.0 || abs(currentData.pitch) > 70.0 || abs(currentData.roll) > 70.0);

        // Gửi dữ liệu đầy đủ vào hàng đợi dataQueue
        xQueueSend(dataQueue, &currentData, 0);

        // Nếu phát hiện ngã, gửi tín hiệu vào dataAlertQueue để Task cảnh báo xử lý
        if (currentData.isFall) {
            bool alertSignal = true;
            xQueueSend(dataAlertQueue, &alertSignal, 0);
        }

        // Lấy mẫu định kỳ mỗi 500ms
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// 3. Task cảnh báo (Chạy trên Core 1)
void vTaskAlert(void *pvParameters) {
    bool alertTriggered = false;

    while (1) {
        // Lắng nghe tín hiệu từ hàng đợi dataAlertQueue
        if (xQueueReceive(dataAlertQueue, &alertTriggered, portMAX_DELAY) == pdPASS) {
            if (alertTriggered) {
                // Tự động bật còi và đèn cảnh báo khi ngã
                digitalWrite(ledPin, HIGH);
                digitalWrite(buzzerPin, HIGH);
                
                // Giữ trạng thái cảnh báo trong 1.5 giây
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
        }
    }
}