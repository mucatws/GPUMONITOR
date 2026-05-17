# GPUMONITOR
Vai monitorar sua GPU

credits to reavolk, made this inspired on his CAGT, go check it out.

# 🖥 GPU Monitor v1.1 — C++

Utilitário de linha de comando para monitorar GPUs **NVIDIA**, **AMD** e **Intel** em tempo real, com suporte a **Linux** e **Windows**.

---

## Suporte por fabricante

| Fabricante | Linux | Windows | Dados disponíveis |
|------------|-------|---------|-------------------|
| NVIDIA     | ✅ NVML (completo) | ✅ NVML + DXGI | Temp, fan, VRAM, utilização, driver |
| AMD        | ✅ sysfs amdgpu | ⚠ DXGI (básico) | Temp, fan, VRAM, consumo, utilização |
| Intel      | ✅ sysfs i915 | ⚠ DXGI (básico) | Nome, VRAM dedicada |

> No Windows, NVIDIA com drivers instalados fornece dados completos via NVML. AMD e Intel ficam limitados ao que o DXGI expõe (nome e VRAM).

---

## Requisitos

### Linux
- Compilador **g++ 11+** com suporte a C++17
- `pciutils` instalado (`lspci`) para identificar nomes de GPU
- **NVIDIA:** pacote `libnvidia-ml` (carregado dinamicamente — sem ele o monitor ainda funciona, mas sem dados NVIDIA)
- **AMD:** módulo `amdgpu` carregado (`lsmod | grep amdgpu`)
- **Intel:** módulo `i915` carregado (`lsmod | grep i915`)

### Windows
- **MSVC 2019+** ou **MinGW/MSYS2**
- SDK do Windows instalado (para `dxgi.h` e `setupapi.h`)
- **NVIDIA:** drivers instalados (o `nvml.dll` vem junto com eles)

---

## Compilação

### Linux — sem NVML (AMD, Intel, NVIDIA básico)
```bash
g++ -std=c++17 -O2 -pthread -o gpu_monitor gpu_monitor.cpp -ldl
```

### Linux — com NVML (NVIDIA completo)
```bash
# Ubuntu/Debian
sudo apt install libnvidia-compute-$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | cut -d. -f1)

# Arch Linux
sudo pacman -S cuda

# Compilar
g++ -std=c++17 -O2 -pthread -o gpu_monitor gpu_monitor.cpp -ldl -lnvidia-ml
```

### Windows — MSVC (Developer Command Prompt)
```cmd
cl /std:c++17 /O2 /EHsc gpu_monitor.cpp /Fe:gpu_monitor.exe
```

### Windows — MinGW / MSYS2
```bash
g++ -std=c++17 -O2 -pthread -o gpu_monitor.exe gpu_monitor.cpp -ldxgi -ld3d11
```

---

## Execução

### Linux
```bash
# Usuário comum — funciona para maioria dos dados
./gpu_monitor

# Com root — libera acesso total a hwmon e sysfs em distros restritas
sudo ./gpu_monitor
```

### Windows
```cmd
gpu_monitor.exe
```

> No Windows, execute como Administrador para garantir acesso completo ao registro e aos dados de driver.

---

## Interface

```
╔══════════════════════════════════════════════════╗
║          🖥  GPU MONITOR v1.1 — C++              ║
║     NVIDIA • AMD • Intel | Windows + Linux        ║
╚══════════════════════════════════════════════════╝
  14:22:05

  2 GPU(s) detectada(s)

  ── GPU 0 ────────────────────────────────────
  ● NVIDIA
  Nome       : NVIDIA GeForce RTX 4070
  Driver     : 545.29.06
  Temperatura: 58°C  OK
  Cooler     : 42%
  GPU Util   : [||||||||.............]  38%
  VRAM Total : 12.0 GB
  VRAM Uso   : [|||||||||............] 4.1 GB / 12.0 GB

  ── GPU 1 ────────────────────────────────────
  ● AMD
  Nome       : AMD Radeon RX 6600
  Driver     : 6.7.8
  PCI ID     : 0x73ff
  Temperatura: 61°C  OK
  Cooler     : 1850 RPM (35%)
  GPU Util   : [|||..................]  18%
  VRAM Total : 8.0 GB
  VRAM Uso   : [||||................] 1.9 GB / 8.0 GB
  Consumo    : 72 W

  OPÇÕES
  [1] Atualizar
  [2] Monitorar ao vivo (a cada 2s)
  [0] Sair
```

---

## Opções

### `[1]` Atualizar

Recarrega todos os dados das GPUs e redesenha a tela.

---

### `[2]` Monitorar ao vivo

Atualiza automaticamente a cada **2 segundos**.

```
  [Enter para voltar ao menu]
```

> Pressione **Enter** para voltar ao menu principal. Não é necessário Ctrl+C.

---

### `[0]` Sair

Encerra o programa, liberando a conexão com NVML se estiver ativa.

---

## Cores dos indicadores

### Temperatura
| Cor | Faixa | Status |
|-----|-------|--------|
| 🟢 Verde | < 75°C | OK |
| 🟡 Amarelo | 75°C – 89°C | ⚠ QUENTE |
| 🔴 Vermelho | ≥ 90°C | ⚠ CRÍTICO |

### Barras de uso (GPU Util e VRAM)
| Cor | Faixa |
|-----|-------|
| 🟢 Verde | < 60% |
| 🟡 Amarelo | 60% – 84% |
| 🔴 Vermelho | ≥ 85% |

---

## Solução de problemas

**"Nenhuma GPU detectada"**
- Linux: verifique se `lspci` está instalado (`sudo apt install pciutils`)
- Linux AMD: confirme que o módulo está ativo — `lsmod | grep amdgpu`
- Linux Intel: confirme que o módulo está ativo — `lsmod | grep i915`
- Linux NVIDIA sem NVML: tente compilar com `-lnvidia-ml` (veja seção de compilação)

**Temperatura ou fan aparecem como "N/A"**
- O sensor pode não estar exposto via hwmon nessa GPU/kernel
- Tente executar com `sudo` — alguns hwmon requerem root

**VRAM aparece como 0 no Windows com AMD/Intel**
- Esperado: o DXGI reporta apenas VRAM dedicada e, em iGPUs, esse valor pode ser zero
- Para dados completos no AMD no Windows, ferramentas como GPU-Z ou ADL SDK são necessárias
