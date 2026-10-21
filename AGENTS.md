# Карта проекта и обязательные правила разработки (RepublicMP / TesmioLoader)

> **ОБЯЗАТЕЛЬНОЕ ПРАВИЛО ДЛЯ АГЕНТА (MANDATORY RULE):**
> 1. Перед внесением любых изменений в код, UI, хуки, сохранения или сетевую синхронизацию изучи релевантные разделы этого файла (`AGENTS.md`).
> 2. **При каждом добавлении, удалении или изменении файлов, хуков, RVA-адресов, структур данных, форматов сохранений или экранов меню ты ОБЯЗАН автоматически обновить этот файл (`d:\SovietRepublic MP\AGENTS.md`) в рамках того же шага до завершения ответа пользователю.**
> 3. Всегда проверяй отсутствие мёртвого кода (неиспользуемых функций, `#define`, переменных) после рефакторинга.

---

## 1. Сборка, развёртывание и запуск игры

- **Сборка всего проекта (`tesmioloader.dll`, `dinput8.dll`, `tesmiolauncher.exe`, `RepublicMP.dll`):**
  ```powershell
  cmd.exe /c "d:\SovietRepublic MP\TesmioLoader\build.bat"
  ```
- **Выходной артефакт плагина:**
  `d:\SovietRepublic MP\TesmioLoader\build\plugins\RepublicMP.dll`
- **Обязательные точки развёртывания после каждой сборки:**
  1. `d:\SovietRepublic MP\local\test-host\plugins\RepublicMP.dll`
  2. `d:\SovietRepublic MP\local\test-client\plugins\RepublicMP.dll`
  3. `d:\SovietRepublic MP\steam_workshop\RepublicMP.dll`
  4. `d:\SovietRepublic MP\steam_workshop\content\TesmioLoader\build\plugins\RepublicMP.dll`
- **Правило запуска `SOVIET64.exe` из агента:**
  - Запускать через `CreateProcessW` с `si.lpDesktop = "WinSta0\\Default"`, `IsDaemon: true`, и **обязательно** держать процесс через `Wait-Process -Id $pidLaunched` внутри фоновой задачи (`WaitMsBeforeAsync: 1500`), иначе по завершении синхронной команды Job Object закроет игру.
- **Логи для диагностики:**
  - `d:\SovietRepublic MP\local\test-host\tesmioloader.log` (основной лог хуков, игры и креш-репортов SEH)
  - `d:\SovietRepublic MP\local\test-host\RepublicMP_Diagnostics.log` (и `.prev.log` — детальный лог UI, сессии, `MOD_SYNC`, телеметрии Lockstep и VEH-дампов)

---

## 2. Архитура модулей `TesmioLoader/RepublicMP`

### `core/` — Ядро плагина и рендер-перехват
- [`main.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/core/main.cpp) — Точка входа плагина (`TsmPluginInit`, `TsmPluginStart`), порядок инициализации всех подсистем.
- [`version.h`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/core/version.h) — Версия мода (`REPUBLICMP_VERSION`) и номер сетевого протокола (`REPUBLICMP_PROTOCOL`).
- [`present_hook.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/core/present_hook.cpp) / [`imgui_hook.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/core/imgui_hook.cpp) — Перехват `IDXGISwapChain::Present`, инициализация ImGui, вызов `NativeMenu_DrawOverlay()` и `TwoStates_DrawOverlay()`.
- [`diag_log.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/core/diag_log.cpp) — Логирование в `RepublicMP_Diagnostics.log`, подавление VEH внутри защищённых SEH-блоков (`DiagLog_BeginSafeSeh` / `DiagLog_EndSafeSeh`), периодический дамп состояния (`TwoStates`, `ModSync`, `Lockstep`).

### `ui/` — Нативный интерфейс игры и локализация
- [`native_menu.h`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/ui/native_menu.h) / [`native_menu.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/ui/native_menu.cpp) — Полная интеграция кооператива в нативный интерфейс `SOVIET64.exe`:
  - **Главное меню (`Hook_MainMenuBuild`, `RVA 0x261897 -> 0x28AF60`):**
    - Встраивает кнопку `КООПЕРАТИВ` в корень главного меню (и переименовывает дублирующую кнопку `0x259` в `Загрузить игру`, если есть `saved_last`).
    - Иерархия страниц (`NativeMenuPage`):
      - `NATIVE_PAGE_ROOT` -> `NATIVE_PAGE_COOP_HUB` (`Создать сервер`, `Подключиться`, `Настройки коопа`, `<< Назад`)
      - `NATIVE_PAGE_HOST` (`Продолжить кооп`, `Кооп-сохранения`, `Импорт одиночного`, `Новая своя игра`, `Новая кампания`, `Настройки`, `<< Назад`)
      - `NATIVE_PAGE_JOIN` (список друзей Steam онлайн, быстрый вход по сохранённому адресу, ручной ввод IP/SteamID, вставка из буфера)
      - `NATIVE_PAGE_SETTINGS` (`Режим: 1/2 ССР`, `Казна: 100% / 50-50`, `Погранзона`, `Карта: 20x20 с границей / Вся без границ`, переключатель авто-синка модов `Моды: Общие (Авто) / Только Хоста / Из сейва`, `Проверка DLC`, `Язык`)
  - **Левая игровая боковая панель `☭` (`Hook_SystemTabBuild`, `RVA 0x31EAF8 -> 0x337450`):**
    - Добавляет кнопку `КООПЕРАТИВ` с советской иконкой `editor/mp_icon_coop.png` внизу системной вкладки `☭`.
    - При клике открывает подвкладку «Кооператив» (через `SYSTAB_SUBTAB_IDX_OFF = -2` и временный NOP заголовка `0x337D7B`), где отрисовывает однородные 9-slice кнопки (`DrawSidebarButton`) с квадратными иконками `96x96` (`editor/mp_icon_*.png`, `editor/settings_save.png`).
    - Содержит кнопку быстрого сохранения хоста **«Сохранить кооп-игру»** (`TwoStates_TriggerQuickCoopSave()`), перевод средств (`50 000 РУБ / $`), прыжок камеры к партнёру (`[F6]`), пинг (`[Alt+ЛКМ]`), переключение 3D-границы (`[F7]`), ре-синк и остановку/отключение.
- [`lang.h`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/ui/lang.h) — Переключение языка (`MP_LANG_RU` / `MP_LANG_EN`), автоопределение по первому пункту главного меню игры и сохранение в `plugins\republicmp_settings.ini` (`[ui] lang=ru/en`).

### `session/` — Сессия, режим «Два государства», сохранения и настройки
- [`two_states.h`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/session/two_states.h) / [`two_states.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/session/two_states.cpp):
  - **Режимы:** `COOP_MODE_ONE_STATE` (0 — общая республика и казна) и `COOP_MODE_TWO_STATES` (1 — Западная ССР у Хоста `X < 0`, Восточная ССР у Клиента `X > 0`, общая погранзона `[-borderHalfWidth .. +borderHalfWidth]`).
  - **Раздельная экономика (`2 государства`):**
    - Хранит `g_stateRub[2]` и `g_stateUsd[2]`.
    - Подменяет `WorldRubles()` / `WorldDollars()` вокруг выполнения команд (`TwoStates_BeginCommand` / `TwoStates_EndCommand`), вокруг UI/инструментов (`TwoStates_UiBegin/End`, `TwoStates_ToolMoneyBegin/End`) и внутри 5 inline-хуков симуляции (`H_BldEcon`, `H_CustomsBorder`, `H_CustomsVehicle`, `H_ConstructStep`, `H_VehicleSale`).
    - Привязывает ID построенных зданий (`g_buildingOwners`) и купленной техники (`g_vehicleOwners`) к государству-владельцу.
  - **Иерархия и разделение сохранений (`Save Bank Swapping`):**
    - `TwoStates_MountSaveBank(bool wantCoop)` атомарно переключает `media_soviet\save` между `media_soviet\save_sp` (одиночные) и `media_soviet\save_coop` (кооперативные, с маркером `.republicmp_coop_bank`).
    - Автоматически очищает временные папки синхронизации `mp_syncsave_*` в главном меню.
    - При импорте одиночного сейва (`TwoStates_SetImportingSpSave(true)`) копирует выбранный сейв из `save_sp` в `save_coop` внутри `TwoStates_BeforeAnyWorldLoad()`, не затрагивая оригинал.
  - **Перехват сохранения движка (`H_MasterSave`, `RVA 0x42CC70`):**
    - Перенаправляет автоматическое сохранение при выходе из кооп-игры (`"saved_last"`) в `"save/coop_last_session"` внутри банка `save_coop`, чтобы не перезаписывать одиночный `saved_last` и всегда сохранять прогресс для кнопки «Продолжить кооп».
    - При отсутствии `script.ini` в целевой папке сохранения временно выставляет `*(BYTE*)(world + 0xFC90) = 1`, чтобы `C3D_TERRAIN::Save` записал все базовые файлы рельефа.
    - Отключает упаковку в `.zip` (`SAVE_ZIP_AFTER_FLAG_OFFSET = 0` и патч `RVA 0x42CCD9` `0x74 -> 0xEB`) для кооп-сохранений.
    - Перед вызовом `o_masterSave(world)` записывает суммарный бюджет (`g_stateRub[0] + g_stateRub[1]`) в мир, а сразу после — сохраняет `republicmp_coop.ini` (`WriteCoopSaveMetaInternal`).
  - **Метаданные кооп-сейва (`republicmp_coop.ini` внутри папки сохранения):**
    - Хранит `coop_mode`, `border_half_width_m`, `expand_borders`, балансы в копейках/центах (`state1_rub_cents`, `state2_rub_cents`, `state1_usd_cents`, `state2_usd_cents`), торговую статистику и списки `building_owners` / `vehicle_owners`.
    - При загрузке кооп-сейва (`ReadCoopSaveMetaInternal`) сохранённые в нём режим и бюджеты имеют **абсолютный приоритет** над настройками стартового капитала в меню.
  - **Глобальные настройки (`plugins\republicmp_settings.ini`):**
    - `TwoStates_LoadSettings()` / `TwoStates_SaveSettings()` сохраняют `mode`, `full_starting_capital`, `border_half_width_m`, `expand_borders` (по умолчанию `0` — стандартная карта 20×20 с границами), `show_3d_border`, `dlc_check`, `last_join_address`, `lang`.
- [`save_sync.h`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/session/save_sync.h) / [`save_sync.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/session/save_sync.cpp) — Рукопожатие `SessionHello`, синхронизация модов и плагинов (`MSG_MOD_SYNC`), упаковка и передача мира от хоста клиенту (`mp_syncsave_host` -> `mp_syncsave_incoming`), запуск Lockstep-сессии.
- [`mod_sync.h`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/session/mod_sync.h) / [`mod_sync.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/session/mod_sync.cpp) — Полная синхронизация модов `Steam Workshop`, локальных `workshop_wip` и геймплейных плагинов `TesmioLoader`:
  - Парсит `header.bin`, `headerE.bin` (заголовок `0x260` байт, записи по `0x80` байт со строками `<WorkshopID>/<name>` и `MIRRORZ_<WorkshopID>/<name>`) для автоматического определения всех Workshop-зданий и техники, реально задействованных в сохранении.
  - Перехватывает `SOVIET_WORKSHOP_MANAGER::RefreshItems` (`RVA 0x8407C0`, указатель на менеджер `*(void**)(g_exeBase + 0x9E22B8)` = `WORLD_RVA + 0xD3A8`): в кооп-сессии фильтрует и сортирует список модов у Хоста и Клиента в идентичном порядке (`SafeFilterAndSortWorkshopMgr` через in-place swap partition с сохранением исключённых элементов в хвосте `g_hiddenTailCount` и восстановлением через `SafeRestoreWorkshopMgr`), исключая смещение индексов в Lockstep и утечки памяти.
  - Через `ModSync_BeforeWorldLoad(isHostStart, saveDir)` (вызывается из `TwoStates_BeforeAnyWorldLoad()` на главном потоке перед `o_loadWorld`) автоматически докачивает недостающие у Хоста моды из `saveDir` через `ISteamUGC`, активирует фильтр модов Хоста сразу при старте кооп-мира и безопасно выполняет `Hook_WorkshopRefresh` на главном потоке после докачки новых модов.
  - Все установленные моды включены автоматически без ручного прокликивания списков. Поддерживает 3 режима авто-синхронизации (`ModPackMode` в настройках коопа): `MOD_PACK_ALL_SHARED` (по умолчанию: общий пул всех модов Хоста, Клиента и сейва с двусторонней авто-докачкой), `MOD_PACK_HOST_ONLY` (все моды Хоста и сейва) и `MOD_PACK_SAVE_ONLY` (только моды из сейва).
  - Автоматически подписывает и докачивает недостающие моды у Хоста и Клиента через `ISteamUGC::SubscribeItem` / `DownloadItem` с отображением прогресса на экране синхронизации, а для локальных `workshop_wip` или удалённых из Steam модов автоматически передаёт архив напрямую по сети во временную песочницу `media_soviet\workshop_wip\<id>` (с маркером `.republicmp_session_temp`, очищаемым после выхода).
  - Через сервис `TSM_SERVICE_PLUGINS` в [`tesmioloader.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/src/tesmioloader.cpp) управляет горячим включением/отключением геймплейных плагинов (`resources`, `deposits`, `needs`, `depletion`, `accumulator`, `daynight`, `walking`, `cities`, `easystart`) без перезапуска игры и синхронизирует их `.ini`-конфиги во временную песочницу клиента `plugins\.mp_session_ini\`.

### `determinism/` и `lockstep/` — Детерминированная симуляция и синхронизация команд
- [`determinism.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/determinism/determinism.cpp) — Фиксированный шаг симуляции, `HookedLoadWorld` (вызывает `TwoStates_BeforeAnyWorldLoad()` до `o_loadWorld()` и `TwoStates_OnAnyWorldLoaded(session)` после), `HookedSimStep`.
- [`sim_patches.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/determinism/sim_patches.cpp) — Патчи детерминизма (`HookedLate` на `0x30D1A0`, перехват оконных вызовов и видимости зданий).
- [`commands.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/commands.cpp) — Очередь Lockstep-команд, обёрнутая в `TwoStates_BeginCommand` / `TwoStates_EndCommand`.
- [`building_switches.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/building_switches.cpp) — Синхронизация переключателей в окнах зданий и нижнем меню мира (`kWorldRanges` на `0xAF8..0xB0C`, где `world + 0xB08` читается/пишется как 1 байт без захвата буфера `FormatMoney` на `world + 0xB0A`); исключает все заголовки `std::vector` внутри `BUILDING` (`kViewWords`, по данным `BUILDING::~BUILDING()` на `0x164BB0`) и `SLOT` (`0x00..0x48`), а также распознаёт heap-указатели в диапазоне `4 GB .. 1 TB` (`high >= 1 && high < 0x100`).
- [`game_calls.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/game_calls.cpp) — Перехват оконных вызовов (`LS_CMD_GAME_CALL` через 32-байтные трамплины), включая 4 кнопки приглашения жителей/специалистов в жилые дома в `0x6F8CC0` (`CALL_INVITE_CITIZENS` на `0x6FEE1E`, `0x6FF31A`, `0x6FF52D`, `0x6FFA4E` -> `0x3FFAD0`, с детерминированным спавном через `InviteCitizensNow` и `0x837EB0`).
- [`route_tools.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/route_tools.cpp) — Синхронизация маршрутных инструментов и переселения граждан (`RouteTools_WorkerRelocateTool` на `0x2B8CE0` и `ApplyWorkerRelocate` через 5-аргументный `0x83C6A0` и `0x83CA50`).
- [`brush_tools.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/brush_tools.cpp) и [`local_builds.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/world_sync/local_builds.cpp) — Синхронизация кистей рельефа, выравнивания земли под превью здания в `2BADE0` (перехват записи в строительную книгу `0x2FDB00` на `0x3AABCD`, `0x3AACE3`, `0x2CB854`, `0x2CB942` через `LS_CMD_LEDGER`) и сброс `TerrainHook_LockstepFlush()` перед постановкой здания/дороги (`LS_CMD_PLACE`, `LS_CMD_SEGMENT`, `LS_CMD_UPGRADE`).

---

## 3. Справочник ключевых RVA и смещений (`SOVIET64.exe`, PE `6A3EB6AD`)

| Назначение | RVA / Смещение | Примечание |
| :--- | :--- | :--- |
| `WORLD_RVA` (`WORLD_THIS_RVA`) | `0x9D4F10` | Глобальный объект мира и главного меню (`menuObj`) |
| `SAVE_NAME_FIELD_OFFSET` | `world + 0x9F8` | `char[120]`: путь сохранения (`"save/<name>"` или `"saved_last"`) |
| `WORLD_FORCE_TERRAIN_SAVE_OFF` | `world + 0xFC90` | `BYTE`: `1` заставляет `C3D_TERRAIN::Save` записать `script.ini` и карты высот |
| `WORLD_UNLIMITED_MONEY_OFFSET` | `world + 0x578` | `int`: `!= 0` — режим неограниченных денег (`Late` на `0x30DAA2` каждый кадр сбрасывает баланс в `100000000.0`) |
| `WORLD_USD_OFFSET` / `RUB` | `world + 0x580` / `+0x588` | `double`: баланс долларов и рублей |
| `SAVE_TRIGGER_FLAG_OFFSET` | `world + 0x14D35` | `BYTE`: `1` — запрос сохранения на следующем кадре |
| `SAVE_ZIP_AFTER_FLAG_OFFSET` | `world + 0x14D36` | `BYTE`: `0` — не упаковывать в 7z/zip |
| `RVA_MASTER_SAVE` | `0x42CC70` | Главная функция сохранения мира (`H_MasterSave`, 19 байт пролога) |
| `RVA_MASTER_SAVE_AUTOSAVE_JE` | `0x42CCD9` | `0x74 -> 0xEB`: отключение `AUTOSAVE_TO_ZIP` в коопе |
| `RVA_LOAD_WORLD` | `0x430FC0` | Главная функция загрузки мира (`HookedLoadWorld`, 18 байт пролога) |
| `SAVE_NAME_DAT_RVA` | `0x9962B0` | `char[120]`: загружаемый путь (`"save\\<name>"`) |
| `LOAD_TRIGGER_DAT_RVA` | `0x993C66` | `BYTE`: `1` — триггер загрузки мира |
| `TERRAIN_FLAG_DAT_RVA` | `0x995298` | `BYTE`: `0` при загрузке сейва/карты по имени |
| `MENU_CALL_SITE_RVA` | `0x261897 -> 0x28AF60` | Перехват сборки главного меню (`Hook_MainMenuBuild`) |
| `SYSTAB_CALL_SITE_RVA` | `0x31EAF8 -> 0x337450` | Перехват левой боковой панели `☭` (`Hook_SystemTabBuild`) |
| `SYSTAB_ROOT_TITLE_RVA` | `0x337D7B` | 6-байтный `call` отрисовки заголовка «Меню параметров» |
| `RVA_BLD_ECON` | `0x173320` | Автозакупка ресурсов зданием и импорт/экспорт ЖКХ |
| `RVA_CUSTOMS_BORDER` | `0x1B7D50` | Пограничная торговля таможни |
| `RVA_CUSTOMS_VEHICLE` | `0x1B9A40` | Погрузка/разгрузка транспорта на таможне/в порту |
| `RVA_CONSTRUCT_STEP` | `0x159070` | Оплата шага строительства/земляных работ |
| `RVA_VEHICLE_SALE` | `0x3E0730` | Продажа техники / доставка за границу |
| `WORKSHOP_MGR_PTR_RVA` | `0x9E22B8` | Указатель `SOVIET_WORKSHOP_MANAGER*` (`WORLD_RVA + 0xD3A8`; `0x9E7750` — это `std::vector<VEHICLE_TYPE>`, не трогать!) |
| `RVA_WORKSHOP_REFRESH` | `0x8407C0` | `SOVIET_WORKSHOP_MANAGER::RefreshItems` (`Hook_WorkshopRefresh`, 16 байт пролога) |
| `RVA_WORKER_RELOCATE_TOOL` | `0x2B8CE0` | Инструмент переселения жителей/заключённых (`RouteTools_WorkerRelocateTool`) |
| `RVA_RELOCATE_CITIZEN` | `0x83C6A0` | Переселение жителя `(world, srcBld, dstBld, testOnly, onlyEducated)` (5 аргументов!) |
| `RVA_RELOCATE_INMATE` | `0x83CA50` | Переселение заключённого `(world, inmateCitizenPtr, dstBld)` |
| `RVA_SPAWN_CITIZEN` | `0x837EB0` | Спавн жителя в здании `(world, bld, 1, minAge, maxAge, eduLevel, assignFlat)` |
| `RVA_INVITE_CITIZENS_SITES` | `0x6FEE1E`, `0x6FF31A`, `0x6FF52D`, `0x6FFA4E` | Кнопки приглашения иммигрантов/специалистов в жилом доме (`0x6F8CC0` -> `0x3FFAD0`) |
| `LEDGER_RVA` | `0x2FDB00` | Запись расхода в строительную книгу (`world + 0x11330` USD / `+0x11360` RUB; сайты `0x3AABCD`, `0x3AACE3`, `0x2CB854`, `0x2CB942`) |

---

## 4. Критические инварианты и подводные камни (Pitfalls)

1. **Запрет смешивания `__try` и C++ объектов с деструкторами (`MSVC error C2712`):**
   - Проект собирается с флагом `/EHsc`. Любой блок `__try / __except` **обязан** находиться в отдельной `static` функции, использующей только POD-типы (без `std::string`, `std::ostringstream`, `std::filesystem::path` в той же функции).
2. **Кэш управляемых текстур `C3D_MIDDLEPOINT` (`GetOrLoadSidebarIcon`):**
   - При выгрузке мира или возврате в главное меню движок освобождает текстуры, созданные через `IMPORT_CREATE_TEX_RVA` (`0x86C4C0`).
   - Всегда вызывай `ResetSidebarIcons()` при входе в главное меню и смене состояния `worldLoaded`, а перед использованием кэшированного `texPtr` проверяй `ReadablePtr(cached, 64) && *(void**)cached == g_expectedTexVtbl`.
3. **Буфер `C3D_INPUT::GetMouseSolid` (`0x86C868`):**
   - Функция записывает 12 байт (`C3DVECTOR3: x, y, z`). Всегда передавай массив минимум `float mousePt[4]`, иначе произойдёт перезапись стека.
4. **Неприкосновенность базовых карт (`media_soviet\terrains` и `dlc*\terrains`):**
   - Функция `TwoStates_PatchSaveScriptIni` должна вызываться **только** для папок сохранений (`save\...` / `save/...`) и только когда включён `g_expandBorders`. Никогда не патчить `script.ini` внутри папок `terrains/`.
5. **Локаль чисел с плавающей точкой (`LC_NUMERIC`):**
   - В русской локали игры `LC_NUMERIC` может менять точку на запятую. В `republicmp_coop.ini` все денежные суммы хранятся исключительно в целых копейках/центах (`long long`, `_strtoi64` / `<<`).
6. **Фильтрация `SOVIET_WORKSHOP_MANAGER` без утечек памяти и гонок потоков:**
   - Указатель `SOVIET_WORKSHOP_MANAGER*` хранится по адресу `*(void**)(g_exeBase + 0x9E22B8)` (`WORLD_RVA + 0xD3A8`). Ни в коем случае не путать с `0x9E7750` (`std::vector<VEHICLE_TYPE>`).
   - Никогда не перезаписывай исключённые указатели `SOVIET_WORKSHOP_ITEM*` в `SOVIET_WORKSHOP_MANAGER` без сохранения их в хвосте `[begin + kept .. begin + total)` (`g_hiddenTailCount`), и всегда вызывай `SafeRestoreWorkshopMgr` перед `o_workshopRefresh` и при завершении сессии. Вызов `o_workshopRefresh` допустим **только** из главного потока (через `ModSync_BeforeWorldLoad`).
7. **Версии ABI интерфейсов в `steam_api64.dll` (`STEAMUGC_INTERFACE_VERSION012`, `SteamNetworking005`, `SteamUser020`, `SteamFriends017`):**
   - Плоские функции `SteamAPI_ISteam*` в `steam_api64.dll` игры скомпилированы строго под `STEAMUGC_INTERFACE_VERSION012` и `SteamNetworking005` (используется в `C3DDLL64.dll`). Запрос более новой версии (`016`, `SteamNetworking006` и т.д.) приводит к смещению слотов vtable и Access Violation (`steam_api64.dll + 0x1323`). Никогда не вызывай `SteamAPI_Init()` из `DllMain`/`TsmPluginStart` — всегда дожидайся `SteamAPI_GetHSteamUser() != 0`.
8. **Защита от инжекта в лаунчер настроек `SETUPAPPLICATION SOVIET.exe`:**
   - При запуске из Steam сначала стартует `SETUPAPPLICATION SOVIET.exe`, который также загружает `C3DDLL64.dll` -> `dinput8.dll`. В `dinput8_proxy.cpp` и `tesmioloader.cpp` обязательно проверяй `IsSoviet64Process()` (`_stricmp(exeName, "SOVIET64.exe") == 0`), иначе хуки RVA `SOVIET64.exe` вызовут краш в `SETUPAPPLICATION SOVIET.exe`.
9. **Бинарный формат `header.bin` и `headerE.bin`:**
   - Заголовок файла занимает ровно `0x260` (608) байт, после чего идут записи ровно по `0x80` (128) байт. В начале каждой записи (`+0x00..+0x63`) лежит ASCII-строка идентификатора типа (`<WorkshopID>/<item_name>` или `MIRRORZ_<WorkshopID>/<item_name>`).
10. **Режим «Неограниченные деньги» (`world + 0x578 != 0`) и `TwoStates` / `TakeMoney`:**
   - В `SOVIET64.exe` внутри `Late` (`0x30DAA2..0x30DAB9`) при `*(int*)(world + 0x578) != 0` движок на каждом кадре принудительно перезаписывает `world + 0x580` и `world + 0x588` значением `100,000,000.0` (`0x4197D78400000000`). Поэтому при `IsUnlimitedMoney()` нельзя делить капитал 50/50 или складывать `g_stateRub[0] + g_stateRub[1] = 200,000,000.0`, а `TakeMoney` не должен генерировать `LS_CMD_MONEY`.
11. **Сигнатура `C3D_SOUNDMANAGER::PlaySound2D` (`IMPORT_PLAY_SOUND_RVA = 0x86D2A8`):**
   - `GetValueString` (`0x86CD18`) возвращает `const char* sndPath` (а не указатель на звук). В `PlaySound2D` (`?PlaySound2D@C3D_SOUNDMANAGER@@QEAA_NPEAD_NM0@Z`) первым аргументом (`rcx`) передаётся `*(void**)(g_exeBase + 0x9E4C18)` (`SOUND_ENGINE_PTR_RVA`), вторым (`rdx`) — `sndPath`, третьим (`r8b`) — `true`, четвёртым (`xmm3`) — `1.0f`, пятым (`[rsp+0x20]`) — `nullptr`.
12. **Структура выбранного инструмента `world + 0xD428` и векторы `BUILDING` в `building_switches.cpp`:**
   - По адресу `*(void**)(world + 0xD428)` хранится указатель на запись меню (`0x2D0` байт), где по смещению `+0x00` лежит встроенный массив `char name[]` (`(const char*)sel`), а **не** `const char*` указатель. Разыменование `*(const char**)sel` приводит к чтению ASCII-символов как адреса и SEH-исключению.
   - В `BUILDING` (`0x1420` байт) все заголовки `std::vector` (например, `building + 0xBC0..0xBD7` — вектор жильцов `std::vector<Citizen*>`, `+0xBD8..0xBEF` — вектор заключённых) обязаны быть исключены в `kViewWords` в `building_switches.cpp`, а `PointerLike(U64)` обязан учитывать низкие сегменты кучи Windows x64 (`high >= 1 && high < 0x100`), иначе изменение размера вектора жильцов на одном ПК отправит младшие 32 бита указателя `_Mylast` как `LS_CMD_BUILDING_SWITCH` и повредит указатель кучи на другом ПК (`ACCESS_VIOLATION` на `0x6FE57C`).
   - Смещение `world + 0xB08` в `building_switches.cpp` — это 1-байтовая настройка; начиная с `world + 0xB0A` функция `FormatMoney` (`0x336420`) пишет форматированную строку стоимости во время превью строительства дорог и зданий.
