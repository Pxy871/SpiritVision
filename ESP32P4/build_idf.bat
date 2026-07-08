@echo off
rem ESP-IDF v5.5.4 build helper for this machine.
rem NOTE: the EIM activation profile still points at C:\Espressif which no
rem longer exists; paths below reflect the actual install on drive D.
set MSYSTEM=
set MSYSTEM_PREFIX=
rem Some agent-launched shells do not inherit Windows CPU architecture vars.
rem ESP-IDF derives its tools platform from Python's platform.machine(), which
rem returns empty on Windows when PROCESSOR_ARCHITECTURE is missing.
if "%PROCESSOR_ARCHITECTURE%"=="" set PROCESSOR_ARCHITECTURE=AMD64
set IDF_PATH=D:\RYH\updedate_app\esp32\ben_ti\v5.5.4\esp-idf
set IDF_TOOLS_PATH=D:\Espressif\tools
set IDF_PYTHON_ENV_PATH=D:\Espressif\tools\python\v5.5.4\venv
set ESP_ROM_ELF_DIR=D:\Espressif\tools\esp-rom-elfs\20241011
set IDF_PYTHON=D:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe
rem esp_wifi_remote/esp_hosted Kconfig relies on this (orsource "idf_v$ESP_IDF_VERSION/...");
rem normally exported by IDF activate scripts which this helper bypasses.
set ESP_IDF_VERSION=5.5
set PATH=D:\Espressif\tools\python\v5.5.4\venv\Scripts;D:\Espressif\tools\cmake\3.30.2\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;D:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;%PATH%
"%IDF_PYTHON%" "%IDF_PATH%\tools\idf.py" %*
