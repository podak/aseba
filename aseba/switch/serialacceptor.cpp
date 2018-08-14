#include "serialacceptor.h"
#include <algorithm>
#include <charconv>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/beast/core/bind_handler.hpp>
#include "log.h"

#include <initguid.h>
#include <devguid.h>   // for GUID_DEVCLASS_PORTS and GUID_DEVCLASS_MODEM
#include <winioctl.h>  // for GUID_DEVINTERFACE_COMPORT
#include <setupapi.h>
#include <cfgmgr32.h>


namespace mobsya {

serial_acceptor_service::serial_acceptor_service(boost::asio::io_context& io_service)
    : boost::asio::detail::service_base<serial_acceptor_service>(io_service)
    , m_active_timer(io_service)
    , m_strand(io_service.get_executor()) {

    mLogInfo("Serial monitoring service: started");
}


void serial_acceptor_service::shutdown() {
    mLogInfo("Serial monitoring service: Stopped");
    m_active_timer.cancel();
}


void serial_acceptor_service::register_request(request& r) {
    m_active_timer.expires_from_now(boost::posix_time::millisec(1));
    m_active_timer.async_wait(boost::asio::bind_executor(
        m_strand, boost::bind(&serial_acceptor_service::on_active_timer, this, boost::placeholders::_1)));
}

void serial_acceptor_service::on_active_timer(const boost::system::error_code& ec) {
    if(ec)
        return;
    std::unique_lock<std::mutex> lock(m_req_mutex);
    this->handle_request_by_active_enumeration();
    if(!m_requests.empty()) {
        m_active_timer.expires_from_now(boost::posix_time::seconds(1));  // :(
        m_active_timer.async_wait(boost::asio::bind_executor(
            m_strand, boost::bind(&serial_acceptor_service::on_active_timer, this, boost::placeholders::_1)));
    }
}

static std::string device_instance_identifier(DEVINST deviceInstanceNumber) {
    std::string str(MAX_DEVICE_ID_LEN + 1, 0);
    if(::CM_Get_Device_IDA(deviceInstanceNumber, str.data(), MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) {
        return {};
    }
    for(char& c : str)
        c = std::toupper(c, std::locale());
    return str;
}
static usb_device_identifier device_id_from_interface_id(const std::string& str) {
    auto value = [&str](const char* prefix) {
        uint16_t res = 0;
        if(auto needle = str.find(prefix); needle != std::string::npos) {
            auto start = needle + strlen(prefix);
            std::from_chars(str.data() + start, str.data() + str.size(), res, 16);
        }
        return res;
    };
    return {value("VID_"), value("PID_")};
}

static std::string get_com_portname(HDEVINFO info_set, PSP_DEVINFO_DATA device_info) {
    const HKEY key = ::SetupDiOpenDevRegKey(info_set, device_info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if(key == INVALID_HANDLE_VALUE)
        return {};
    auto value = [&key](const char* subk) -> std::string {
        DWORD dataType = 0;
        std::string str(MAX_PATH + 1, 0);
        DWORD bytesRequired = MAX_PATH;
        for(;;) {
            const LONG ret = ::RegQueryValueExA(key, subk, nullptr, &dataType,
                                               reinterpret_cast<PBYTE>(str.data()), &bytesRequired);
            if(ret == ERROR_MORE_DATA) {
                str.resize(bytesRequired / sizeof(str) + 1, 0);
                continue;
            } else if(ret == ERROR_SUCCESS) {
                if(dataType == REG_SZ)
                    return str;
                else if(dataType == REG_DWORD)
                    return fmt::format("COM{}", *(PDWORD(str.data())));
            }
            return {};
        }
    };
    std::string port_name = value("PortName");
    if(port_name.empty())
        port_name = value("PortNumber");
    ::RegCloseKey(key);
    return port_name;
}

void serial_acceptor_service::handle_request_by_active_enumeration() {
    const HDEVINFO deviceInfoSet =
        ::SetupDiGetClassDevs(&GUID_DEVINTERFACE_COMPORT, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if(deviceInfoSet == INVALID_HANDLE_VALUE)
        return;

	std::vector<std::string> known_devices;

    SP_DEVINFO_DATA deviceInfoData;
    ::memset(&deviceInfoData, 0, sizeof(deviceInfoData));
    deviceInfoData.cbSize = sizeof(deviceInfoData);

    DWORD index = 0;
    while(::SetupDiEnumDeviceInfo(deviceInfoSet, index++, &deviceInfoData)) {
        SP_DEVINFO_DATA deviceInfoData;
        ::memset(&deviceInfoData, 0, sizeof(deviceInfoData));
        deviceInfoData.cbSize = sizeof(deviceInfoData);

        DWORD index = 0;
        while(::SetupDiEnumDeviceInfo(deviceInfoSet, index++, &deviceInfoData)) {
            auto& req = m_requests.front();
            const auto str = device_instance_identifier(deviceInfoData.DevInst);
            const auto id = device_id_from_interface_id(str);
            const auto& devices = req.acceptor.compatible_devices();
            if(std::find(std::begin(devices), std::end(devices), id ) == std::end(devices)) {
                mLogTrace("device not compatible : {:#06X}-{:#06X} ", id.vendor_id, id.product_id);
                continue;
            }
            known_devices.push_back(str);
            if(std::find(m_known_devices.begin(), m_known_devices.end(), str) != m_known_devices.end())
                continue;

            const auto port_name = get_com_portname(deviceInfoSet, &deviceInfoData);
            mLogTrace("{}", str);
            mLogTrace("device : {:#06X}-{:#06X} on {}", id.vendor_id, id.product_id, port_name);
            boost::system::error_code ec;
            req.d.open(port_name, ec);
            if(!ec) {
				auto handler = std::move(req.handler);
				const auto executor = boost::asio::get_associated_executor(handler, req.acceptor.get_executor());
				m_requests.pop();
				boost::asio::post(executor, boost::beast::bind_handler(handler, boost::system::error_code{}));
			}
        }
        ::SetupDiDestroyDeviceInfoList(deviceInfoSet);
    }
    m_known_devices = known_devices;
}


void serial_acceptor_service::construct(implementation_type&) {}
void serial_acceptor_service::destroy(implementation_type&) {}

serial_acceptor::serial_acceptor(boost::asio::io_context& io_service,
                                 std::initializer_list<usb_device_identifier> devices)
    : boost::asio::basic_io_object<serial_acceptor_service>(io_service) {

    std::copy(std::begin(devices), std::end(devices),
              std::back_inserter(this->get_implementation().compatible_devices));
}

const std::vector<usb_device_identifier>& serial_acceptor::compatible_devices() const {
    return this->get_implementation().compatible_devices;
}


}  // namespace mobsya
