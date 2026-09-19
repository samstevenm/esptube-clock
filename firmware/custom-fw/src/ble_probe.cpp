// ble_probe.cpp — NOT part of the product build. Compiled only in env:esptube_bleprobe to measure
// the flash/RAM cost of a NimBLE "Nordic UART" service (the shell over Bluetooth LE / Web Bluetooth).
#ifdef ESPTUBE_BLE_PROBE
#include <NimBLEDevice.h>
static NimBLECharacteristic* g_tx;
void bleProbeBegin() {
    NimBLEDevice::init("esptube");
    NimBLEServer* srv = NimBLEDevice::createServer();
    NimBLEService* svc = srv->createService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    svc->createCharacteristic("6E400002-B5A3-F393-E0A9-E50E24DCCA9E", NIMBLE_PROPERTY::WRITE);
    g_tx = svc->createCharacteristic("6E400003-B5A3-F393-E0A9-E50E24DCCA9E", NIMBLE_PROPERTY::NOTIFY);
    svc->start(); NimBLEDevice::getAdvertising()->addServiceUUID(svc->getUUID()); NimBLEDevice::startAdvertising();
}
void bleProbeSend(const char* s) { if (g_tx) { g_tx->setValue((const uint8_t*)s, strlen(s)); g_tx->notify(); } }
#endif
