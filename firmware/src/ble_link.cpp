#include "ble_link.h"
#include "config.h"
#include "storage.h"
#include "epd_driver.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>

static const char *SVC_UUID = "6f4c0001-8a2e-4c91-b7d4-1e5a9c30f001";
static const char *CTRL_UUID = "6f4c0002-8a2e-4c91-b7d4-1e5a9c30f001";
static const char *IMG_UUID = "6f4c0003-8a2e-4c91-b7d4-1e5a9c30f001";

static const uint8_t OP_SET_IDLE = 0x01;
static const uint8_t OP_BEGIN = 0x02;
static const uint8_t OP_SHOW = 0x03;
static const uint8_t OP_GET = 0x04;
static const uint8_t OP_SET_PANEL = 0x05;
static const uint8_t OP_RESULT = 0x81;
static const uint8_t OP_INFO = 0x82;

static const uint8_t ST_OK = 0;
static const uint8_t ST_BAD = 1;
static const uint8_t ST_BUSY = 2;
static const uint8_t ST_SHORT = 3;
static const uint8_t ST_OVERFLOW = 4;

#ifndef ESP_GATT_PERM_READ_ENCRYPTED
#define ESP_GATT_PERM_READ_ENCRYPTED (1 << 1)
#define ESP_GATT_PERM_WRITE_ENCRYPTED (1 << 5)
#endif

static BLEServer *s_server = nullptr;
static BLECharacteristic *s_ctrl = nullptr;
static bool s_connected = false;
static bool s_stopping = false;
static uint16_t s_connId = 0;
static bool s_imgOpen = false;
static uint32_t s_imgGot = 0;
static volatile bool s_show = false;
static volatile bool s_busy = false;

static void notifyBytes(const uint8_t *data, size_t len) {
    if (!s_connected || !s_ctrl) return;
    s_ctrl->setValue(data, len);
    s_ctrl->notify();
}

static void notifyResult(uint8_t op, uint8_t status) {
    uint8_t msg[3] = {OP_RESULT, op, status};
    notifyBytes(msg, sizeof(msg));
}

static void notifyInfo() {
    uint16_t idle = storageIdleMinutes();
    uint8_t msg[8] = {
        OP_INFO,
        (uint8_t)(idle & 0xFF),
        (uint8_t)((idle >> 8) & 0xFF),
        storagePanelRev(),
        (uint8_t)(EPD_WIDTH & 0xFF),
        (uint8_t)((EPD_WIDTH >> 8) & 0xFF),
        (uint8_t)(EPD_HEIGHT & 0xFF),
        (uint8_t)((EPD_HEIGHT >> 8) & 0xFF),
    };
    notifyBytes(msg, sizeof(msg));
}

static uint16_t readU16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t readU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void onControl(const uint8_t *data, size_t len) {
    bleNoteActivity();
    if (len < 1) return;
    uint8_t op = data[0];
    if (s_busy && op != OP_GET) {
        notifyResult(op, ST_BUSY);
        return;
    }
    switch (op) {
    case OP_SET_IDLE: {
        if (len != 3) {
            notifyResult(op, ST_BAD);
            return;
        }
        uint16_t minutes = readU16(data + 1);
        if (minutes > IDLE_MINUTES_MAX) {
            notifyResult(op, ST_BAD);
            return;
        }
        storageSetIdleMinutes(minutes);
        notifyResult(op, ST_OK);
        notifyInfo();
        break;
    }
    case OP_BEGIN: {
        if (len != 9) {
            notifyResult(op, ST_BAD);
            return;
        }
        uint32_t total = readU32(data + 1);
        uint16_t width = readU16(data + 5);
        uint16_t height = readU16(data + 7);
        if (width != EPD_WIDTH || height != EPD_HEIGHT || total != (uint32_t)COLOR_BUF_LEN) {
            notifyResult(op, ST_BAD);
            return;
        }
        s_imgOpen = true;
        s_imgGot = 0;
        notifyResult(op, ST_OK);
        break;
    }
    case OP_SHOW:
        if (!s_imgOpen || s_imgGot != (uint32_t)COLOR_BUF_LEN) {
            notifyResult(op, ST_SHORT);
            return;
        }
        s_imgOpen = false;
        __sync_synchronize();
        s_show = true;
        notifyResult(op, ST_OK);
        break;
    case OP_GET:
        notifyInfo();
        break;
    case OP_SET_PANEL:
        if (len != 2 || data[1] > 1) {
            notifyResult(op, ST_BAD);
            return;
        }
        storageSetPanelRev(data[1]);
        notifyResult(op, ST_OK);
        notifyInfo();
        break;
    default:
        notifyResult(op, ST_BAD);
        break;
    }
}

static void onImage(const uint8_t *data, size_t len) {
    bleNoteActivity();
    if (!s_imgOpen || len == 0) return;
    if (s_imgGot + len > (uint32_t)COLOR_BUF_LEN) {
        s_imgOpen = false;
        notifyResult(OP_BEGIN, ST_OVERFLOW);
        return;
    }
    memcpy(colorBuf + s_imgGot, data, len);
    s_imgGot += len;
}

class ControlCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        onControl(c->getData(), c->getLength());
    }
};

class ImageCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        onImage(c->getData(), c->getLength());
    }
};

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *server) override {
        s_connected = true;
        s_connId = server->getConnId();
        bleNoteActivity();
        Serial.println("[BLE] connected");
        notifyInfo();
    }
    void onDisconnect(BLEServer *server) override {
        (void)server;
        s_connected = false;
        s_imgOpen = false;
        Serial.println("[BLE] disconnected");
        if (!s_stopping && BLEDevice::getInitialized()) {
            BLEDevice::startAdvertising();
        }
    }
};

class LinkSecurity : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t) override {}
    bool onSecurityRequest() override { return true; }
    bool onConfirmPIN(uint32_t) override { return true; }
    bool onAuthorizationRequest(uint16_t, uint16_t, bool) override { return true; }
#if defined(CONFIG_BLUEDROID_ENABLED)
    void onAuthenticationComplete(esp_ble_auth_cmpl_t desc) override {
        Serial.printf("[BLE] paired=%d\n", desc.success);
        if (desc.success) notifyInfo();
    }
#endif
#if defined(CONFIG_NIMBLE_ENABLED)
    void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
        Serial.println("[BLE] paired");
        (void)desc;
        notifyInfo();
    }
#endif
};

static ServerCallbacks s_serverCb;
static ControlCallbacks s_controlCb;
static ImageCallbacks s_imageCb;
static LinkSecurity s_securityCb;

void bleStart() {
    s_connected = false;
    s_imgOpen = false;
    s_imgGot = 0;
    s_show = false;

    BLEDevice::init("InkSight398");
    BLEDevice::setMTU(517);
    BLESecurity::setAuthenticationMode(true, false, true);
    BLESecurity::setCapability(ESP_IO_CAP_NONE);
    BLESecurity::setInitEncryptionKey();
    BLESecurity::setRespEncryptionKey();
    // 不在连接瞬间强制配对。系统设置没有本服务，配完就会断开。
    // 手机应用写入加密特征时再配对。
    BLESecurity::setForceAuthentication(false);
    BLEDevice::setSecurityCallbacks(&s_securityCb);

    s_server = BLEDevice::createServer();
    s_server->setCallbacks(&s_serverCb);

    BLEService *service = s_server->createService(SVC_UUID);
    const uint16_t encPerm = ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED;

    s_ctrl = service->createCharacteristic(
        CTRL_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
    s_ctrl->setAccessPermissions(encPerm);
    s_ctrl->setCallbacks(&s_controlCb);
    s_ctrl->addDescriptor(new BLE2902());

    BLECharacteristic *image = service->createCharacteristic(
        IMG_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    image->setAccessPermissions(encPerm);
    image->setCallbacks(&s_imageCb);

    service->start();

    // 广播包只有 31 字节。名称和 128 位 UUID 不能同时放入。
    // 名称放在广播包里，系统和应用才能在不读扫描响应时看到 InkSight398。
    BLEAdvertising *adv = BLEDevice::getAdvertising();
    BLEAdvertisementData advData;
    advData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    advData.setName("InkSight398");
    BLEAdvertisementData scanData;
    scanData.setCompleteServices(BLEUUID(SVC_UUID));
    adv->setAdvertisementData(advData);
    adv->setScanResponseData(scanData);
    adv->setScanResponse(true);
    BLEDevice::startAdvertising();
    Serial.println("[BLE] advertising InkSight398");
}

void bleStop() {
    s_stopping = true;
    s_imgOpen = false;
    if (s_connected && s_server) {
        s_server->disconnect(s_connId);
        delay(200);
    }
    s_connected = false;
    if (BLEDevice::getInitialized()) {
        BLEDevice::deinit(true);
    }
    s_server = nullptr;
    s_ctrl = nullptr;
    s_stopping = false;
}

bool bleConnected() { return s_connected; }

bool bleTakeShowRequest() {
    if (!s_show) return false;
    s_show = false;
    s_busy = true;
    return true;
}

void bleNotifyShowDone(bool ok) {
    s_busy = false;
    notifyResult(OP_SHOW, ok ? ST_OK : ST_BAD);
}
