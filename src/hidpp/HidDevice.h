#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>

struct hid_device_info;
struct hid_device_;

namespace hidpp {

struct DeviceInfo {
    uint16_t    vendorId;
    uint16_t    productId;
    std::wstring manufacturer;
    std::wstring product;
    std::wstring serialNumber;
    std::string  path;
    uint16_t    usagePage;
    uint16_t    usage;
    int          interfaceNumber;
};

class HidDevice {
public:
    HidDevice();
    ~HidDevice();

    HidDevice(const HidDevice&)            = delete;
    HidDevice& operator=(const HidDevice&) = delete;
    HidDevice(HidDevice&&)                 = default;

    static std::vector<DeviceInfo> enumerate(uint16_t vid = 0, uint16_t pid = 0);

    bool open(const std::string& path);
    bool open(uint16_t vid, uint16_t pid, uint16_t usagePage = 0, uint16_t usage = 0);
    void close();
    bool isOpen() const;

    bool write(const std::vector<uint8_t>& data);
    bool read(std::vector<uint8_t>& data, int timeoutMs = 1000);

    std::wstring lastError() const;

    uint16_t vendorId()  const { return m_vid; }
    uint16_t productId() const { return m_pid; }

private:
    hid_device_* m_dev  = nullptr;
    uint16_t     m_vid  = 0;
    uint16_t     m_pid  = 0;
    std::wstring m_lastError;
};

} // namespace hidpp
