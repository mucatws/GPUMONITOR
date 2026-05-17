#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <functional>
#include <cstring>
#include <cstdlib>
#include <ctime>       // FIX: necessário para localtime_r / localtime_s
#include <mutex>       // FIX: proteção de saída em modo monitor
 
#ifdef _WIN32
    #define PLATFORM_WINDOWS
    #include <windows.h>
    #include <dxgi.h>
    #include <setupapi.h>
    #pragma comment(lib, "dxgi.lib")
    #pragma comment(lib, "setupapi.lib")
#else
    #define PLATFORM_LINUX
    #include <dlfcn.h>
    #include <fstream>
    #include <dirent.h>
    #include <sys/types.h>
    #include <cerrno>   // FIX: errno para mensagens de erro
#endif
 
// ─── NVML (carregado dinamicamente — funciona com ou sem NVIDIA) ────────────
// FIX: nvmlDevice_t original tinha apenas 4 bytes — tamanho real é opaque pointer (8 bytes em 64-bit)
// Usar ponteiro opaco void* é mais seguro que um struct de tamanho fixo arbitrário
typedef void* nvmlDevice_t;
typedef enum { NVML_SUCCESS = 0 } nvmlReturn_t;
 
// FIX: assinatura de nvmlDeviceGetMemoryInfo corrigida — a API real usa uma struct, não 3 ponteiros separados
typedef struct {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
} nvmlMemory_t;
 
typedef nvmlReturn_t (*nvmlInit_t)();
typedef nvmlReturn_t (*nvmlDeviceGetCount_t)(unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetHandleByIndex_t)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*nvmlDeviceGetName_t)(nvmlDevice_t, char*, unsigned int);
typedef nvmlReturn_t (*nvmlDeviceGetTemperature_t)(nvmlDevice_t, unsigned int, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetFanSpeed_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetMemoryInfo_t)(nvmlDevice_t, nvmlMemory_t*);   // FIX
typedef nvmlReturn_t (*nvmlDeviceGetUtilizationRates_t)(nvmlDevice_t, unsigned int*, unsigned int*);
typedef nvmlReturn_t (*nvmlSystemGetDriverVersion_t)(char*, unsigned int);
typedef nvmlReturn_t (*nvmlShutdown_t)();
 
// ─── Cores ─────────────────────────────────────────────────────────────────
namespace Color {
    const std::string RESET   = "\033[0m";
    const std::string BOLD    = "\033[1m";
    const std::string DIM     = "\033[2m";
    const std::string RED     = "\033[91m";
    const std::string YELLOW  = "\033[93m";
    const std::string GREEN   = "\033[92m";
    const std::string BLUE    = "\033[94m";
    const std::string CYAN    = "\033[96m";
    const std::string MAGENTA = "\033[95m";
    const std::string ORANGE  = "\033[38;5;208m";
}
 
// ─── Estrutura de dados da GPU ──────────────────────────────────────────────
struct GPUInfo {
    std::string vendor;
    std::string name;
    std::string driver_version;
    int         temp_c        = -1;
    int         fan_rpm       = -1;
    int         fan_pct       = -1;
    int         gpu_util_pct  = -1;
    uint64_t    vram_total_mb = 0;
    uint64_t    vram_used_mb  = 0;
    uint64_t    vram_free_mb  = 0;
    int         power_w       = -1;
    std::string pci_id;
    bool        available     = false;
};
 
// ─── Mutex para saída (modo monitor usa thread separada) ────────────────────
static std::mutex g_cout_mutex;
 
// ─── Helpers ────────────────────────────────────────────────────────────────
std::string temp_color(int t) {
    if (t < 0)   return Color::DIM;
    if (t >= 90) return Color::RED;
    if (t >= 75) return Color::YELLOW;
    return Color::GREEN;
}
 
std::string make_bar(double pct, int width = 25) {
    std::string col = (pct >= 85) ? Color::RED : (pct >= 60) ? Color::YELLOW : Color::GREEN;
    int filled = std::clamp(static_cast<int>(pct / 100.0 * width), 0, width);
    return "[" + col + std::string(filled, '|') + Color::DIM +
           std::string(width - filled, '.') + Color::RESET + "]";
}
 
std::string fmt_mb(uint64_t mb) {
    std::ostringstream oss;
    if (mb >= 1024)
        oss << std::fixed << std::setprecision(1) << static_cast<double>(mb) / 1024.0 << " GB";
    else
        oss << mb << " MB";
    return oss.str();
}
 
// FIX: system("clear/cls") → sequência ANSI, sem fork/processo extra
void clear_screen() {
    std::cout << "\033[2J\033[H" << std::flush;
}
 
// FIX: localtime_r / localtime_s — thread-safe (substitui std::localtime)
std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf {};
#ifdef PLATFORM_WINDOWS
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(2) << tm_buf.tm_hour << ":"
        << std::setw(2) << tm_buf.tm_min  << ":"
        << std::setw(2) << tm_buf.tm_sec;
    return oss.str();
}
 
// ─── Backend NVML (NVIDIA, dinâmico) ────────────────────────────────────────
class NVMLBackend {
public:
    bool  loaded = false;
    void* lib    = nullptr;
 
    nvmlInit_t                      nvmlInit                    = nullptr;
    nvmlShutdown_t                  nvmlShutdown                = nullptr;
    nvmlDeviceGetCount_t            nvmlDeviceGetCount          = nullptr;
    nvmlDeviceGetHandleByIndex_t    nvmlDeviceGetHandleByIndex  = nullptr;
    nvmlDeviceGetName_t             nvmlDeviceGetName           = nullptr;
    nvmlDeviceGetTemperature_t      nvmlDeviceGetTemperature    = nullptr;
    nvmlDeviceGetFanSpeed_t         nvmlDeviceGetFanSpeed       = nullptr;
    nvmlDeviceGetMemoryInfo_t       nvmlDeviceGetMemoryInfo     = nullptr;
    nvmlDeviceGetUtilizationRates_t nvmlDeviceGetUtilizationRates = nullptr;
    nvmlSystemGetDriverVersion_t    nvmlSystemGetDriverVersion  = nullptr;
 
    bool load() {
#ifdef PLATFORM_LINUX
        const char* libs[] = { "libnvidia-ml.so.1", "libnvidia-ml.so", nullptr };
        for (int i = 0; libs[i]; i++) {
            lib = dlopen(libs[i], RTLD_LAZY);
            if (lib) break;
        }
#else
        lib = static_cast<void*>(LoadLibraryA("nvml.dll"));
        if (!lib)
            lib = static_cast<void*>(LoadLibraryA(
                "C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll"));
#endif
        if (!lib) return false;
 
#ifdef PLATFORM_LINUX
    #define LOAD(fn) fn = reinterpret_cast<fn##_t>(dlsym(lib, #fn))
#else
    #define LOAD(fn) fn = reinterpret_cast<fn##_t>(GetProcAddress(static_cast<HMODULE>(lib), #fn))
#endif
        LOAD(nvmlInit);
        LOAD(nvmlShutdown);
        LOAD(nvmlDeviceGetCount);
        LOAD(nvmlDeviceGetHandleByIndex);
        LOAD(nvmlDeviceGetName);
        LOAD(nvmlDeviceGetTemperature);
        LOAD(nvmlDeviceGetFanSpeed);
        LOAD(nvmlDeviceGetMemoryInfo);
        LOAD(nvmlDeviceGetUtilizationRates);
        LOAD(nvmlSystemGetDriverVersion);
#undef LOAD
 
        if (!nvmlInit || nvmlInit() != NVML_SUCCESS) return false;
        loaded = true;
        return true;
    }
 
    std::vector<GPUInfo> query() {
        std::vector<GPUInfo> gpus;
        if (!loaded) return gpus;
 
        unsigned int count = 0;
        if (nvmlDeviceGetCount(&count) != NVML_SUCCESS || count == 0)
            return gpus;
 
        char driver[256] = {};
        if (nvmlSystemGetDriverVersion)
            nvmlSystemGetDriverVersion(driver, sizeof(driver));
 
        for (unsigned int i = 0; i < count; i++) {
            // FIX: nvmlDevice_t agora é void* — passa endereço corretamente
            nvmlDevice_t dev = nullptr;
            if (nvmlDeviceGetHandleByIndex(i, &dev) != NVML_SUCCESS) continue;
 
            GPUInfo g;
            g.vendor         = "NVIDIA";
            g.available      = true;
            g.driver_version = driver[0] ? driver : "N/A";
 
            char name[256] = {};
            if (nvmlDeviceGetName(dev, name, sizeof(name)) == NVML_SUCCESS)
                g.name = name;
 
            unsigned int temp = 0;
            if (nvmlDeviceGetTemperature(dev, 0 /*NVML_TEMPERATURE_GPU*/, &temp) == NVML_SUCCESS)
                g.temp_c = static_cast<int>(temp);
 
            unsigned int fan = 0;
            if (nvmlDeviceGetFanSpeed(dev, &fan) == NVML_SUCCESS)
                g.fan_pct = static_cast<int>(fan);
 
            // FIX: usa nvmlMemory_t struct — assinatura correta da API NVML
            if (nvmlDeviceGetMemoryInfo) {
                nvmlMemory_t mem {};
                if (nvmlDeviceGetMemoryInfo(dev, &mem) == NVML_SUCCESS) {
                    g.vram_total_mb = mem.total / (1024ULL * 1024ULL);
                    g.vram_used_mb  = mem.used  / (1024ULL * 1024ULL);
                    g.vram_free_mb  = mem.free  / (1024ULL * 1024ULL);
                }
            }
 
            unsigned int gpu_util = 0, mem_util = 0;
            if (nvmlDeviceGetUtilizationRates &&
                nvmlDeviceGetUtilizationRates(dev, &gpu_util, &mem_util) == NVML_SUCCESS)
                g.gpu_util_pct = static_cast<int>(gpu_util);
 
            gpus.push_back(g);
        }
        return gpus;
    }
 
    ~NVMLBackend() {
        if (loaded && nvmlShutdown) nvmlShutdown();
#ifdef PLATFORM_LINUX
        if (lib) dlclose(lib);
#else
        if (lib) FreeLibrary(static_cast<HMODULE>(lib));
#endif
    }
};
 
// ─── Backend Linux sysfs (AMD + Intel + fallback) ──────────────────────────
#ifdef PLATFORM_LINUX
std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string val;
    std::getline(f, val);
    return val;
}
 
// FIX: run_cmd com tratamento de erro explícito e sem usar string como buffer de
//      tamanho fixo para saídas potencialmente longas
std::string run_cmd(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    char buf[512];
    std::string out;
    while (fgets(buf, sizeof(buf), p))
        out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}
 
// FIX: safe_stoull/stoi — evita exceções não tratadas ao ler arquivos sysfs
//      que podem conter lixo ou estar vazios numa race condition
static uint64_t safe_stoull(const std::string& s, uint64_t fallback = 0) {
    if (s.empty()) return fallback;
    try { return std::stoull(s); }
    catch (...) { return fallback; }
}
 
static int safe_stoi(const std::string& s, int fallback = -1) {
    if (s.empty()) return fallback;
    try { return std::stoi(s); }
    catch (...) { return fallback; }
}
 
std::vector<GPUInfo> query_sysfs() {
    std::vector<GPUInfo> gpus;
 
    DIR* dir = opendir("/sys/class/drm");
    if (!dir) return gpus;
 
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        // Aceita apenas cardN sem traço (ex: card0, card1 — não card0-HDMI-A-1)
        if (name.find("card") == std::string::npos ||
            name.find('-') != std::string::npos) continue;
 
        std::string base = "/sys/class/drm/" + name + "/device/";
 
        std::string vendor_id = read_file(base + "vendor");
        if (vendor_id.empty()) continue;
 
        GPUInfo g;
        g.available = true;
        g.pci_id    = read_file(base + "device");
 
        if      (vendor_id == "0x10de") g.vendor = "NVIDIA";
        else if (vendor_id == "0x1002") g.vendor = "AMD";
        else if (vendor_id == "0x8086") g.vendor = "Intel";
        else g.vendor = "Desconhecida (" + vendor_id + ")";
 
        // FIX: lspci usa vendor_id sem o 0x; extrai apenas os 4 dígitos hex
        std::string vid_hex = (vendor_id.size() > 2)
                              ? vendor_id.substr(2) : vendor_id;
        g.name = run_cmd("lspci -d " + vid_hex + ": 2>/dev/null | grep -iE 'vga|3d|display' | head -1 | sed 's/.*: //'");
        if (g.name.empty())
            g.name = run_cmd("lspci 2>/dev/null | grep -iE 'vga|3d|display' | head -1 | sed 's/.*: //'");
        if (g.name.empty()) g.name = g.vendor + " GPU";
 
        // Temperatura e cooler via hwmon
        std::string hwmon_base = base + "hwmon/";
        DIR* hdir = opendir(hwmon_base.c_str());
        if (hdir) {
            struct dirent* he;
            while ((he = readdir(hdir)) != nullptr) {
                std::string hname = he->d_name;
                if (hname.find("hwmon") == std::string::npos) continue;
                std::string hbase = hwmon_base + hname + "/";
 
                // FIX: usa safe_stoi para não explodir em sysfs corrompido
                std::string temp_s = read_file(hbase + "temp1_input");
                if (!temp_s.empty()) {
                    int raw = safe_stoi(temp_s, -1000);
                    if (raw > -1000) g.temp_c = raw / 1000;
                }
 
                std::string fan_s = read_file(hbase + "fan1_input");
                int fan_v = safe_stoi(fan_s);
                if (fan_v >= 0) g.fan_rpm = fan_v;
 
                std::string pwm_s = read_file(hbase + "pwm1");
                int pwm_v = safe_stoi(pwm_s);
                if (pwm_v >= 0)
                    g.fan_pct = static_cast<int>(pwm_v * 100.0 / 255.0);
 
                // Consumo em microwatts → watts
                std::string pw_s = read_file(hbase + "power1_average");
                uint64_t pw_v = safe_stoull(pw_s);
                if (pw_v > 0) g.power_w = static_cast<int>(pw_v / 1000000ULL);
            }
            closedir(hdir);
        }
 
        // VRAM (AMD amdgpu sysfs)
        uint64_t vt = safe_stoull(read_file(base + "mem_info_vram_total"));
        uint64_t vu = safe_stoull(read_file(base + "mem_info_vram_used"));
        if (vt > 0) {
            g.vram_total_mb = vt / (1024ULL * 1024ULL);
            g.vram_used_mb  = vu / (1024ULL * 1024ULL);
            g.vram_free_mb  = g.vram_total_mb - g.vram_used_mb;
        }
 
        // GPU utilização (AMD)
        int busy = safe_stoi(read_file(base + "gpu_busy_percent"));
        if (busy >= 0) g.gpu_util_pct = busy;
 
        // Driver version
        if      (g.vendor == "NVIDIA") g.driver_version = run_cmd("nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -1");
        else if (g.vendor == "AMD")    g.driver_version = run_cmd("modinfo amdgpu 2>/dev/null | grep '^version' | awk '{print $2}'");
        else if (g.vendor == "Intel")  g.driver_version = run_cmd("modinfo i915   2>/dev/null | grep '^version' | awk '{print $2}'");
        if (g.driver_version.empty()) g.driver_version = "N/A";
 
        gpus.push_back(g);
    }
    closedir(dir);
    return gpus;
}
#endif // PLATFORM_LINUX
 
// ─── Backend Windows DXGI ──────────────────────────────────────────────────
#ifdef PLATFORM_WINDOWS
std::vector<GPUInfo> query_dxgi() {
    std::vector<GPUInfo> gpus;
 
    IDXGIFactory* factory = nullptr;
    // FIX: CreateDXGIFactory requer o IID explícito — usa __uuidof corretamente
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory),
                                  reinterpret_cast<void**>(&factory))))
        return gpus;
 
    IDXGIAdapter* adapter = nullptr;
    for (UINT i = 0;
         factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND;
         i++) {
        DXGI_ADAPTER_DESC desc {};
        adapter->GetDesc(&desc);
 
        GPUInfo g;
        g.available = true;
 
        // Wide → UTF-8
        char buf[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                             buf, sizeof(buf) - 1, nullptr, nullptr);
        g.name = buf;
 
        switch (desc.VendorId) {
            case 0x10DE: g.vendor = "NVIDIA"; break;
            case 0x1002: g.vendor = "AMD";    break;
            case 0x8086: g.vendor = "Intel";  break;
            default: {
                std::ostringstream oss;
                oss << "Desconhecida (0x" << std::hex << desc.VendorId << ")";
                g.vendor = oss.str();
            }
        }
 
        g.vram_total_mb = desc.DedicatedVideoMemory / (1024ULL * 1024ULL);
 
        // FIX: itera sobre os subkeys de Class 0000, 0001... em vez de hardcodar 0000
        //      (sistemas com múltiplas GPUs têm 0000, 0001, etc.)
        HKEY class_key;
        const char* class_path =
            "SYSTEM\\CurrentControlSet\\Control\\Class\\"
            "{4d36e968-e325-11ce-bfc1-08002be10318}";
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, class_path, 0,
                           KEY_READ | KEY_ENUMERATE_SUB_KEYS, &class_key) == ERROR_SUCCESS) {
            char sub[16];
            DWORD sub_sz = sizeof(sub);
            for (DWORD idx = 0;
                 RegEnumKeyExA(class_key, idx, sub, &sub_sz,
                               nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
                 idx++, sub_sz = sizeof(sub)) {
                HKEY sub_key;
                if (RegOpenKeyExA(class_key, sub, 0, KEY_READ, &sub_key) != ERROR_SUCCESS)
                    continue;
                char ver[256] = {};
                DWORD sz = sizeof(ver);
                if (RegQueryValueExA(sub_key, "DriverVersion", nullptr, nullptr,
                                      reinterpret_cast<LPBYTE>(ver), &sz) == ERROR_SUCCESS) {
                    // Verifica se este subkey pertence ao mesmo adapter pelo DeviceDesc
                    char dev_desc[256] = {};
                    DWORD dd_sz = sizeof(dev_desc);
                    RegQueryValueExA(sub_key, "DriverDesc", nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(dev_desc), &dd_sz);
                    if (g.name.find(dev_desc) != std::string::npos ||
                        std::string(dev_desc).find(g.vendor) != std::string::npos) {
                        g.driver_version = ver;
                        RegCloseKey(sub_key);
                        break;
                    }
                }
                RegCloseKey(sub_key);
            }
            RegCloseKey(class_key);
        }
        if (g.driver_version.empty()) g.driver_version = "N/A";
 
        gpus.push_back(g);
        adapter->Release();
    }
    factory->Release();
    return gpus;
}
#endif // PLATFORM_WINDOWS
 
// ─── Coleta unificada ───────────────────────────────────────────────────────
std::vector<GPUInfo> collect_gpu_info() {
    std::vector<GPUInfo> gpus;
 
    // FIX: NVMLBackend como static com destrutor garante nvmlShutdown no exit
    static NVMLBackend nvml;
    static bool nvml_tried = false;
    if (!nvml_tried) {
        nvml_tried = true;
        nvml.load();
    }
 
    if (nvml.loaded) {
        auto nv = nvml.query();
        for (auto& g : nv) gpus.push_back(g);
    }
 
#ifdef PLATFORM_LINUX
    auto sysfs = query_sysfs();
    for (auto& g : sysfs) {
        // FIX: deduplicação por vendor+name (mais confiável que checar substr do vendor no name)
        bool dup = false;
        for (auto& ex : gpus) {
            if (ex.vendor == g.vendor && ex.name == g.name) {
                dup = true;
                break;
            }
        }
        if (!dup) gpus.push_back(g);
    }
#endif
 
#ifdef PLATFORM_WINDOWS
    if (gpus.empty()) {
        auto dxgi = query_dxgi();
        for (auto& g : dxgi) gpus.push_back(g);
    }
#endif
 
    return gpus;
}
 
// ─── Exibição ───────────────────────────────────────────────────────────────
void print_header() {
    std::cout << Color::BOLD << Color::CYAN;
    std::cout << "╔══════════════════════════════════════════════════╗\n";
    std::cout << "║          🖥  GPU MONITOR v1.1 — C++              ║\n";
    std::cout << "║     NVIDIA • AMD • Intel | Windows + Linux        ║\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n";
    std::cout << Color::RESET;
}
 
void print_vendor_badge(const std::string& vendor) {
    std::string color;
    if      (vendor == "NVIDIA") color = Color::GREEN;
    else if (vendor == "AMD")    color = Color::RED;
    else if (vendor == "Intel")  color = Color::BLUE;
    else                         color = Color::DIM;
    std::cout << color << "  ● " << vendor << Color::RESET;
}
 
void print_gpu(const GPUInfo& g, int idx) {
    std::cout << "\n" << Color::BOLD << Color::CYAN
              << "  ── GPU " << idx << " ────────────────────────────────────\n"
              << Color::RESET;
 
    print_vendor_badge(g.vendor);
    std::cout << "\n";
    std::cout << "  Nome       : " << Color::BOLD  << g.name            << Color::RESET << "\n";
    std::cout << "  Driver     : " << Color::CYAN  << g.driver_version  << Color::RESET << "\n";
    if (!g.pci_id.empty())
        std::cout << "  PCI ID     : " << Color::DIM << g.pci_id << Color::RESET << "\n";
 
    std::cout << "  Temperatura: ";
    if (g.temp_c >= 0) {
        std::cout << temp_color(g.temp_c) << g.temp_c << "°C" << Color::RESET;
        if      (g.temp_c >= 90) std::cout << Color::RED    << " ⚠ CRÍTICO" << Color::RESET;
        else if (g.temp_c >= 75) std::cout << Color::YELLOW << " ⚠ QUENTE"  << Color::RESET;
        else                     std::cout << Color::GREEN   << " OK"         << Color::RESET;
    } else {
        std::cout << Color::DIM << "N/A" << Color::RESET;
    }
    std::cout << "\n";
 
    std::cout << "  Cooler     : ";
    if (g.fan_rpm > 0) {
        std::cout << Color::CYAN << g.fan_rpm << " RPM" << Color::RESET;
        if (g.fan_pct >= 0) std::cout << " (" << g.fan_pct << "%)";
    } else if (g.fan_pct >= 0) {
        std::cout << Color::CYAN << g.fan_pct << "%" << Color::RESET;
    } else {
        std::cout << Color::DIM << "N/A (passivo ou não detectado)" << Color::RESET;
    }
    std::cout << "\n";
 
    std::cout << "  GPU Util   : ";
    if (g.gpu_util_pct >= 0)
        std::cout << make_bar(g.gpu_util_pct) << " " << g.gpu_util_pct << "%\n";
    else
        std::cout << Color::DIM << "N/A\n" << Color::RESET;
 
    std::cout << "  VRAM Total : " << Color::CYAN << fmt_mb(g.vram_total_mb) << Color::RESET << "\n";
    if (g.vram_used_mb > 0 || g.vram_free_mb > 0) {
        double vram_pct = g.vram_total_mb > 0
            ? 100.0 * static_cast<double>(g.vram_used_mb) /
                      static_cast<double>(g.vram_total_mb)
            : 0.0;
        std::cout << "  VRAM Uso   : " << make_bar(vram_pct)
                  << " " << fmt_mb(g.vram_used_mb)
                  << " / " << fmt_mb(g.vram_total_mb) << "\n";
    }
 
    if (g.power_w >= 0)
        std::cout << "  Consumo    : " << Color::YELLOW << g.power_w << " W" << Color::RESET << "\n";
}
 
void print_no_gpu() {
    std::cout << "\n  " << Color::YELLOW << "Nenhuma GPU detectada.\n\n" << Color::RESET;
    std::cout << "  Possíveis causas:\n" << Color::DIM
              << "  • Linux: instale lspci (pciutils) e verifique permissões\n"
              << "  • NVIDIA: instale os drivers ou o pacote libnvidia-ml\n"
              << "  • AMD: verifique se o módulo amdgpu está carregado (lsmod | grep amdgpu)\n"
              << "  • Intel: módulo i915 deve estar ativo\n"
              << Color::RESET;
}
 
void print_menu() {
    std::cout << "\n" << Color::BOLD << "  OPÇÕES\n" << Color::RESET;
    std::cout << "  [1] Atualizar\n";
    std::cout << "  [2] Monitorar ao vivo (a cada 2s)\n";
    std::cout << "  [0] Sair\n";
    std::cout << "\n  Escolha: " << std::flush;
}
 
// ─── Modo monitor ──────────────────────────────────────────────────────────
// FIX: flag de parada + thread de input para saída limpa sem depender de Ctrl+C
static volatile bool g_running = true;
 
void mode_monitor() {
    std::cout << Color::YELLOW
              << "\n  Modo monitor — pressione Enter para sair\n"
              << Color::RESET << std::flush;
 
    std::thread input_thread([]() {
        std::string dummy;
        std::getline(std::cin, dummy);
        g_running = false;
    });
 
    while (g_running) {
        clear_screen();
        {
            std::lock_guard<std::mutex> lock(g_cout_mutex);
            print_header();
            std::cout << Color::DIM << "  Atualizado: " << timestamp() << "\n" << Color::RESET;
            auto gpus = collect_gpu_info();
            if (gpus.empty()) {
                print_no_gpu();
            } else {
                for (int i = 0; i < static_cast<int>(gpus.size()); i++)
                    print_gpu(gpus[i], i);
            }
            std::cout << Color::DIM << "\n  [Enter para voltar ao menu]\n"
                      << Color::RESET << std::flush;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
 
    input_thread.join();
    g_running = true;
}
 
// ─── Main ───────────────────────────────────────────────────────────────────
int main() {
#ifdef PLATFORM_WINDOWS
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode  = 0;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
 
    while (true) {
        clear_screen();
        print_header();
        std::cout << Color::DIM << "  " << timestamp() << "\n" << Color::RESET;
 
        auto gpus = collect_gpu_info();
 
        if (gpus.empty()) {
            print_no_gpu();
        } else {
            std::cout << "\n  " << Color::GREEN << gpus.size()
                      << " GPU(s) detectada(s)" << Color::RESET << "\n";
            for (int i = 0; i < static_cast<int>(gpus.size()); i++)
                print_gpu(gpus[i], i);
        }
 
        print_menu();
 
        std::string choice;
        std::getline(std::cin, choice);
 
        if (choice == "1") {
            continue;
        } else if (choice == "2") {
            mode_monitor();
        } else if (choice == "0") {
            std::cout << Color::CYAN << "\n  Saindo.\n\n" << Color::RESET;
            break;
        }
    }
 
    return 0;
}
