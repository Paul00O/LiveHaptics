#include "HidDevice.h"
#include <hidapi.h>
#include <cstring>

namespace hidpp {

struct HidApiInit {
    HidApiInit()  { hid_init(); }
    ~HidApiInit() { hid_exit(); }
};
static HidApiInit g_hidInit;

HidDevice::HidDevice()  = default;
HidDevice::~HidDevice() { close(); }

std::vector<DeviceInfo> HidDevice::enumerate(uint16_t vid, uint16_t pid) {
    std::vector<DeviceInfo> result;
    hid_device_info* list = hid_enumerate(vid, pid);
    for (hid_device_info* p = list; p; p = p->next) {
        DeviceInfo d;
        d.vendorId        = p->vendor_id;
        d.productId       = p->product_id;
        d.manufacturer    = p->manufacturer_string ? p->manufacturer_string : L"";
        d.product         = p->product_string       ? p->product_string       : L"";
        d.serialNumber    = p->serial_number        ? p->serial_number        : L"";
        d.path            = p->path                 ? p->path                 : "";
        d.usagePage       = p->usage_page;
        d.usage           = p->usage;
        d.interfaceNumber = p->interface_number;
        result.push_back(std::move(d));
    }
    hid_free_enumeration(list);
    return result;
}

bool HidDevice::open(const std::string& path) {
    close();
    m_dev = hid_open_path(path.c_str());
    if (!m_dev) {
        const wchar_t* err = hid_error(nullptr);
        m_lastError = err ? err : L"hid_open_path failed";
        return false;
    }
    hid_set_nonblocking(m_dev, 0);
    return true;
}

bool HidDevice::open(uint16_t vid, uint16_t pid, uint16_t usagePage, uint16_t usage) {
    auto devices = enumerate(vid, pid);
    for (auto& d : devices) {
        if (usagePage && d.usagePage != usagePage) continue;
        if (usage     && d.usage     != usage)     continue;
        if (open(d.path)) {
            m_vid = d.vendorId;
            m_pid = d.productId;
            return true;
        }
    }
    m_lastError = L"No matching device found";
    return false;
}

void HidDevice::close() {
    if (m_dev) {
        hid_close(m_dev);
        m_dev = nullptr;
    }
}

bool HidDevice::isOpen() const { return m_dev != nullptr; }

bool HidDevice::write(const std::vector<uint8_t>& data) {
    if (!m_dev) return false;
    // hidapi expects report ID as first byte
    int ret = hid_write(m_dev, data.data(), data.size());
    if (ret < 0) {
        const wchar_t* err = hid_error(m_dev);
        m_lastError = err ? err : L"hid_write failed";
        return false;
    }
    return true;
}

bool HidDevice::read(std::vector<uint8_t>& data, int timeoutMs) {
    if (!m_dev) return false;
    data.resize(64);
    int ret = hid_read_timeout(m_dev, data.data(), data.size(), timeoutMs);
    if (ret < 0) {
        const wchar_t* err = hid_error(m_dev);
        m_lastError = err ? err : L"hid_read failed";
        return false;
    }
    data.resize(static_cast<size_t>(ret));
    return true;
}

std::wstring HidDevice::lastError() const { return m_lastError; }

} // namespace hidpp
