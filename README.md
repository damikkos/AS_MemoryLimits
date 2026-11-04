<h1 align="center">AS_MemoryLimits</h1>
<p align="center"><strong>Настраиваемые лимиты памяти скриптов DayZ Server</strong></p>

<p align="center">
  <img alt="Релиз v4.0.0" src="https://img.shields.io/badge/релиз-v4.0.0-4c1?style=for-the-badge">
  <img alt="Windows x64" src="https://img.shields.io/badge/Windows-x64-0078D4?style=for-the-badge">
  <img alt="GRAFT ABI 7" src="https://img.shields.io/badge/GRAFT-ABI%207%20%2F%20layout%202-F07533?style=for-the-badge">
  <a href="LICENSE"><img alt="GPL-3.0-or-later" src="https://img.shields.io/badge/лицензия-GPL--3.0--or--later-007EC6?style=for-the-badge"></a>
</p>

<p align="center">
  <a href="#установка">Установка</a> ·
  <a href="#настройки">Настройки</a> ·
  <a href="#сборка-из-исходников">Сборка</a> ·
  <a href="CHANGELOG.md">Изменения</a>
</p>

## О плагине

**AS_MemoryLimits** — серверный C++ плагин для [KR_GRAFTED](https://github.com/KRdayzmodding/KR_GRAFTED). Он увеличивает шесть лимитов памяти скриптов. Значения задаются в `MemoryLimits.ini`. Изменения применяются в памяти процесса; серверный EXE на диске остаётся исходным.

## Установка

1. Остановите сервер. Убедитесь, что [GRAFT](https://github.com/KRdayzmodding/KR_GRAFTED) уже установлен и другие патчи этих лимитов отключены.
2. Скачайте `AS_MemoryLimits-v4.0.0-win-x64.zip` в разделе **Releases**. В ZIP находится каталог `grafted` с DLL и INI.
3. Скопируйте `grafted` из архива в корень сервера. При обновлении сохраните свой `MemoryLimits.ini`, если уже настроили его.
4. Запустите сервер и проверьте журнал GRAFT.

```text
DayZServer/
├── DayZServer_x64.exe
├── hid.dll                              # загрузчик GRAFT
└── grafted/
    ├── AS_MemoryLimits.grafted.dll
    └── MemoryLimits.ini
```

## Настройки

Укажите все шесть значений в секции `[MemoryLimits]`. Единицы — MiB. Принимаются целые числа от исходного лимита до 1024. После изменения файла перезапустите сервер.

| Параметр | Исходный лимит | Значение в комплекте |
| --- | ---: | ---: |
| `GameMB` | 32 | 64 |
| `WorldMB` | 32 | 64 |
| `MissionMB` | 32 | 64 |
| `GameLibMB` | 2 | 16 |
| `InitScriptMB` | 1 | 8 |
| `EngineMB` | 8 | 32 |

Это размеры отдельных буферов, а не общий лимит оперативной памяти сервера. Если INI отсутствует, плагин создаёт его со значениями из таблицы.

## Проверка загрузки

Ищите в журнале GRAFT строку:

```text
[AS_MemoryLimits] all six limits applied; EXE on disk unchanged
```

Предыдущие строки показывают старые и новые размеры. Если в журнале есть `[AS_MemoryLimits] FAILED`, проверьте указанную причину: версию EXE, настройки INI или конфликт с другим патчем.

## Сборка из исходников

Потребуются CMake 3.30+, Ninja, `clang-cl` и окружение Visual Studio для x64 с Windows SDK. Подготовьте копию [исходников GRAFT](https://github.com/KRdayzmodding/KR_GRAFTED) на ревизии `9a55f22540ef2b72e9ecee5672c9acd180392aab`.

Выполните из корня этого репозитория, подставив путь к GRAFT:

```powershell
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl "-DGRAFT_SOURCE_DIR=C:/src/KR_GRAFTED"
cmake --build build/release --target as_memory_limits
```

DLL и INI появятся в `build/release/out/graft/grafted`. Для статической сверки серверного EXE можно запустить `python source/AS_MemoryLimits/tests/verify_current_exe.py <путь-к-DayZServer_x64.exe>`. Скрипт читает EXE без запуска и записи.

## Лицензия

Исходники AS_MemoryLimits распространяются по [GPL-3.0-or-later](LICENSE). Информация о GRAFT и его лицензии приведена в [THIRD_PARTY.md](THIRD_PARTY.md).
