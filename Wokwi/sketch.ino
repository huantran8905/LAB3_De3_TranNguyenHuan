// Lab 3 Đề 3 - Mạch báo cháy ESP32 với FreeRTOS và MQTT

#include <Arduino.h>
#include <DHTesp.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <math.h>
#include <esp_system.h>
#include <string.h>

// Chân GPIO của cảm biến và hai LED mô phỏng bơm, quạt.
constexpr int CHAN_MQ2 = 34;
constexpr int CHAN_DHT22 = 32;
constexpr int CHAN_BOM = 26;
constexpr int CHAN_QUAT = 27;
// Mỗi đầu ra có ngưỡng bật và tắt riêng để tránh nhấp nháy gần ngưỡng.
constexpr int NGUONG_BAT_QUAT = 3800;
constexpr int NGUONG_TAT_QUAT = 3700;
constexpr float NGUONG_BAT_BOM = 55.0f;
constexpr float NGUONG_TAT_BOM = 52.0f;
constexpr uint32_t CHU_KY_DOC_MS = 2000;

// Topic và nội dung gói MQTT phải khớp với dashboard.
#define TOPIC_GOC "lab3/de3/nhom17-4f92bc"
constexpr char TOPIC_DU_LIEU[] = TOPIC_GOC "/telemetry";
constexpr char TOPIC_TRANG_THAI[] = TOPIC_GOC "/state";
constexpr char TOPIC_LENH[] = TOPIC_GOC "/command";

struct MauCamBien {
  uint32_t soThuTu;
  int gasADC;
  float nhietDo;
  bool nhietDoHopLe;
};

struct TrangThaiThietBi {
  bool thuCong;
  bool yeuCauBatBom;
  bool yeuCauBatQuat;
  bool canhBaoGas;
  bool canhBaoNhiet;
  bool nhietDoHopLe;

  bool bomDangBat() const { return canhBaoNhiet || (thuCong && yeuCauBatBom); }
  bool quatDangBat() const { return canhBaoGas || (thuCong && yeuCauBatQuat); }
};

enum class LenhDieuKhien : uint8_t { TuDong, ThuCong, BatBom, TatBom, BatQuat, TatQuat };

// Hai hàng đợi mẫu giữ bản ghi mới nhất cho hai tác vụ nhận độc lập.
QueueHandle_t hangDoiDuLieu;
QueueHandle_t hangDoiCanhBao;
QueueHandle_t hangDoiLenh;
QueueHandle_t hangDoiTrangThai;
DHTesp dht;
WiFiClient tcp;
PubSubClient mqtt(tcp);

// MQ-2 được biểu diễn theo phần trăm thang ADC 12 bit.
float phanTramGas(int gasADC) {
  return gasADC * 100.0f / 4095.0f;
}

bool kiemTraNguong(float giaTri, bool dangCanhBao, float nguongBat, float nguongTat) {
  return dangCanhBao ? giaTri > nguongTat : giaTri >= nguongBat;
}

void xuLyLenh(TrangThaiThietBi &trangThai, LenhDieuKhien lenh) {
  switch (lenh) {
    case LenhDieuKhien::TuDong:
      trangThai.thuCong = trangThai.yeuCauBatBom = trangThai.yeuCauBatQuat = false;
      break;
    case LenhDieuKhien::ThuCong:
      trangThai.thuCong = true;
      break;
    case LenhDieuKhien::BatBom:
    case LenhDieuKhien::TatBom:
      if (trangThai.thuCong) trangThai.yeuCauBatBom = lenh == LenhDieuKhien::BatBom;
      break;
    case LenhDieuKhien::BatQuat:
    case LenhDieuKhien::TatQuat:
      if (trangThai.thuCong) trangThai.yeuCauBatQuat = lenh == LenhDieuKhien::BatQuat;
      break;
  }
}

// Kiểm tra lệnh từ web rồi đưa vào hàng đợi để tác vụ cảnh báo xử lý.
void nhanLenhMQTT(char *chuDe, byte *noiDung, unsigned int doDai) {
  if (strcmp(chuDe, TOPIC_LENH) != 0) return;
  const struct { const char *tenLenh; LenhDieuKhien giaTri; } danhSachLenh[] = {
    {"AUTO", LenhDieuKhien::TuDong}, {"MANUAL", LenhDieuKhien::ThuCong},
    {"PUMP_ON", LenhDieuKhien::BatBom}, {"PUMP_OFF", LenhDieuKhien::TatBom},
    {"FAN_ON", LenhDieuKhien::BatQuat}, {"FAN_OFF", LenhDieuKhien::TatQuat}
  };
  for (const auto &muc : danhSachLenh) {
    if (doDai != strlen(muc.tenLenh) || memcmp(noiDung, muc.tenLenh, doDai) != 0) continue;
    if (xQueueSend(hangDoiLenh, &muc.giaTri, 0) != pdPASS) {
      Serial.println("[TRUYỀN] Hàng đợi lệnh đã đầy");
    }
    break;
  }
}

// Lõi 1 đọc cảm biến mỗi 2 giây và gửi cùng một mẫu đến hai hàng đợi.
void tacVuDocCamBien(void *) {
  uint32_t soThuTu = 0;
  TickType_t mocLayMau = xTaskGetTickCount();
  for (;;) {
    int tongADC = 0;
    for (int i = 0; i < 16; ++i) tongADC += analogRead(CHAN_MQ2);
    const TempAndHumidity ketQuaDHT = dht.getTempAndHumidity();
    const MauCamBien mau{++soThuTu, tongADC / 16, ketQuaDHT.temperature, isfinite(ketQuaDHT.temperature)};
    char nhietDo[32] = "LỖI ĐỌC DHT22";
    if (mau.nhietDoHopLe) snprintf(nhietDo, sizeof(nhietDo), "%.1f C", mau.nhietDo);
    Serial.printf("[ĐỌC - LÕI %d] Mẫu #%lu | Gas: %d ADC (%.1f%%) | Nhiệt độ: %s\n",
                  xPortGetCoreID(), (unsigned long)mau.soThuTu,
                  mau.gasADC, phanTramGas(mau.gasADC), nhietDo);
    xQueueOverwrite(hangDoiCanhBao, &mau);
    xQueueOverwrite(hangDoiDuLieu, &mau);
    vTaskDelayUntil(&mocLayMau, pdMS_TO_TICKS(CHU_KY_DOC_MS));
  }
}

// Lõi 1 điều khiển GPIO. Cảnh báo tự động luôn ưu tiên hơn lệnh thủ công.
void tacVuCanhBao(void *) {
  TrangThaiThietBi trangThai{};
  for (;;) {
    const TrangThaiThietBi trangThaiCu = trangThai;
    MauCamBien mau;
    bool canCapNhat = xQueueReceive(hangDoiCanhBao, &mau, pdMS_TO_TICKS(100)) == pdPASS;
    if (canCapNhat) {
      trangThai.canhBaoGas = kiemTraNguong(mau.gasADC, trangThai.canhBaoGas, NGUONG_BAT_QUAT, NGUONG_TAT_QUAT);
      trangThai.nhietDoHopLe = mau.nhietDoHopLe;
      if (mau.nhietDoHopLe) {
        trangThai.canhBaoNhiet = kiemTraNguong(mau.nhietDo, trangThai.canhBaoNhiet, NGUONG_BAT_BOM, NGUONG_TAT_BOM);
      }
    }

    LenhDieuKhien lenh;
    for (int i = 0; i < 8 && xQueueReceive(hangDoiLenh, &lenh, 0) == pdPASS; ++i) {
      xuLyLenh(trangThai, lenh);
      canCapNhat = true;
    }
    if (!canCapNhat) continue;

    digitalWrite(CHAN_BOM, trangThai.bomDangBat() ? HIGH : LOW);
    digitalWrite(CHAN_QUAT, trangThai.quatDangBat() ? HIGH : LOW);
    if (trangThaiCu.bomDangBat() != trangThai.bomDangBat() || trangThaiCu.quatDangBat() != trangThai.quatDangBat() ||
        trangThaiCu.thuCong != trangThai.thuCong) {
      Serial.printf("[CẢNH BÁO - LÕI %d] %s | Bơm: %s | Quạt: %s\n", xPortGetCoreID(),
                    trangThai.thuCong ? "Thủ công" : "Tự động",
                    trangThai.bomDangBat() ? "BẬT" : "TẮT",
                    trangThai.quatDangBat() ? "BẬT" : "TẮT");
    }
    xQueueOverwrite(hangDoiTrangThai, &trangThai);
  }
}

// Tạo JSON theo định dạng dashboard nhận. Lỗi DHT22 được gửi là null.
bool guiDuLieuMQTT(const MauCamBien &mau) {
  char nhietDo[20] = "null";
  if (mau.nhietDoHopLe) {
    snprintf(nhietDo, sizeof(nhietDo), "%.1f", mau.nhietDo);
  }
  char goiJSON[160];
  snprintf(goiJSON, sizeof(goiJSON), "{\"seq\":%lu,\"gas_adc\":%d,\"gas_pct\":%.2f,\"temp_c\":%s}",
           (unsigned long)mau.soThuTu, mau.gasADC, phanTramGas(mau.gasADC), nhietDo);
  return mqtt.publish(TOPIC_DU_LIEU, goiJSON);
}

bool guiTrangThaiMQTT(const TrangThaiThietBi &trangThai) {
  char goiJSON[160];
  snprintf(goiJSON, sizeof(goiJSON),
           "{\"mode\":\"%s\",\"pump\":%d,\"fan\":%d,\"gas_alarm\":%d,\"heat_alarm\":%d,\"temp_valid\":%d}",
           trangThai.thuCong ? "MANUAL" : "AUTO", trangThai.bomDangBat(), trangThai.quatDangBat(),
           trangThai.canhBaoGas, trangThai.canhBaoNhiet, trangThai.nhietDoHopLe);
  return mqtt.publish(TOPIC_TRANG_THAI, goiJSON);
}

// Lõi 0 phụ trách mạng để việc kết nối lại không cản trở xử lý cảnh báo.
void tacVuTruyenDuLieu(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.begin("Wokwi-GUEST", "", 6);
  mqtt.setServer("broker.hivemq.com", 1883);
  mqtt.setSocketTimeout(2);
  mqtt.setCallback(nhanLenhMQTT);

  char maThietBi[24];
  snprintf(maThietBi, sizeof(maThietBi), "lab3-fire-%08lx", (unsigned long)esp_random());
  uint32_t lanThuWiFi = millis();
  uint32_t lanThuMQTT = millis() - 5000;
  MauCamBien mau{};
  TrangThaiThietBi trangThai{};
  bool choGuiDuLieu = false;
  bool choGuiTrangThai = false;

  for (;;) {
    const uint32_t hienTai = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (hienTai - lanThuWiFi >= 12000) {
        lanThuWiFi = hienTai;
        tcp.stop();
        WiFi.reconnect();
      }
    } else {
      if (!mqtt.connected() && hienTai - lanThuMQTT >= 5000) {
        lanThuMQTT = hienTai;
        if (mqtt.connect(maThietBi) && mqtt.subscribe(TOPIC_LENH)) {
          Serial.printf("[TRUYỀN - LÕI %d] Đã kết nối Wi-Fi và MQTT\n", xPortGetCoreID());
        } else {
          Serial.printf("[TRUYỀN] Chưa kết nối được MQTT, mã lỗi %d\n", mqtt.state());
          tcp.stop();
        }
      }
      if (mqtt.connected() && mqtt.loop()) {
        if (xQueueReceive(hangDoiDuLieu, &mau, 0) == pdPASS) choGuiDuLieu = true;
        if (xQueueReceive(hangDoiTrangThai, &trangThai, 0) == pdPASS) choGuiTrangThai = true;
        if (choGuiDuLieu && guiDuLieuMQTT(mau)) choGuiDuLieu = false;
        if (choGuiTrangThai && guiTrangThaiMQTT(trangThai)) choGuiTrangThai = false;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

// Khởi tạo phần cứng, hàng đợi và gán từng tác vụ vào lõi CPU.
void setup() {
  Serial.begin(115200);
  pinMode(CHAN_BOM, OUTPUT);
  pinMode(CHAN_QUAT, OUTPUT);
  digitalWrite(CHAN_BOM, LOW);
  digitalWrite(CHAN_QUAT, LOW);
  analogReadResolution(12);
  dht.setup(CHAN_DHT22, DHTesp::DHT22);

  hangDoiDuLieu = xQueueCreate(1, sizeof(MauCamBien));
  hangDoiCanhBao = xQueueCreate(1, sizeof(MauCamBien));
  hangDoiLenh = xQueueCreate(8, sizeof(LenhDieuKhien));
  hangDoiTrangThai = xQueueCreate(1, sizeof(TrangThaiThietBi));
  if (!hangDoiDuLieu || !hangDoiCanhBao || !hangDoiLenh || !hangDoiTrangThai) {
    Serial.println("[KHỞI ĐỘNG] Không tạo được hàng đợi");
    return;
  }
  if (xTaskCreatePinnedToCore(tacVuCanhBao, "CanhBao", 4096, nullptr, 3, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(tacVuDocCamBien, "DocCamBien", 4096, nullptr, 2, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(tacVuTruyenDuLieu, "TruyenDuLieu", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
    Serial.println("[KHỞI ĐỘNG] Không tạo được tác vụ");
  }
}

// Công việc chính do các tác vụ FreeRTOS thực hiện.
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
