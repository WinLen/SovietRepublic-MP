# Карта проекта и обязательные правила разработки (RepublicMP / TesmioLoader)

> **ОБЯЗАТЕЛЬНОЕ ПРАВИЛО ДЛЯ АГЕНТА (MANDATORY RULE):**
> 1. Перед внесением любых изменений в код, UI, хуки, сохранения или сетевую синхронизацию изучи релевантные разделы этого файла (`AGENTS.md`).
> 2. **При каждом добавлении, удалении или изменении файлов, хуков, RVA-адресов, структур данных, форматов сохранений или экранов меню ты ОБЯЗАН автоматически обновить этот файл (`d:\SovietRepublic MP\AGENTS.md`) в рамках того же шага до завершения ответа пользователю.**
> 3. Всегда проверяй отсутствие мёртвого кода (неиспользуемых функций, `#define`, переменных) после рефакторинга.
> 4. **При каждом пуше в Git и публикации обновления в Steam Workshop ты ОБЯЗАН инкрементировать версию `REPUBLICMP_VERSION` в [`TesmioLoader/RepublicMP/core/version.h`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/core/version.h) (например, `1.0.1` -> `1.0.2`), а при изменении сетевых сообщений — также инкрементировать `REPUBLICMP_PROTOCOL`, после чего пересобрать `RepublicMP.dll`, развернуть во все целевые пути, загрузить в Steam Workshop (`3823543010`) и запушить коммит и тег `v<REPUBLICMP_VERSION>` в Git (`origin main`).**

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
  - `<GameDir>\RemoteLogs\<Reason>_<YYYYMMDD_HHMMSS>\` — автоматически принятые от Клиента логи (`tesmioloader.log`, `RepublicMP_Diagnostics.log`, `RepublicMP_Diagnostics.prev.log`, `info.txt`) при `DESYNC`, `CRASH`, `DISCONNECT` и `RECONNECT_PREV_CRASH`

---

## 2. Архитура модулей `TesmioLoader/RepublicMP`

### `core/` — Ядро плагина и рендер-перехват
- [`main.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/core/main.cpp) — Точка входа плагина (`TsmPluginInit`, `TsmPluginStart`), порядок инициализации всех подсистем.
- [`version.h`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/core/version.h) — Версия мода (`REPUBLICMP_VERSION`) и номер сетевого протокола (`REPUBLICMP_PROTOCOL`).
- [`present_hook.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/core/present_hook.cpp) / [`imgui_hook.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/core/imgui_hook.cpp) — Перехват `IDXGISwapChain::Present`, инициализация ImGui, вызов `NativeMenu_DrawOverlay()` и `TwoStates_DrawOverlay()`.
- [`diag_log.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/core/diag_log.cpp) — Логирование в `RepublicMP_Diagnostics.log`, подавление VEH внутри защищённых SEH-блоков (`DiagLog_BeginSafeSeh` / `DiagLog_EndSafeSeh`), периодический дамп состояния (`TwoStates`, `ModSync`, `Lockstep`), а также сборка и сохранение бандла клиентских логов (`DiagLog_BuildClientLogBundle` / `DiagLog_SaveRemoteClientLogBundle`) и экстренная отправка логов Хосту прямо из `DiagVectoredExceptionHandler` при фатальном краше Клиента.

### `ui/` — Нативный интерфейс игры и локализация
- [`native_menu.h`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/ui/native_menu.h) / [`native_menu.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/ui/native_menu.cpp) — Полная интеграция кооператива в нативный интерфейс `SOVIET64.exe`:
  - **Главное меню (`Hook_MainMenuBuild`, `RVA 0x261897 -> 0x28AF60`):**
    - Встраивает кнопку `КООПЕРАТИВ` в корень главного меню (и переименовывает дублирующую кнопку `0x259` в `Загрузить игру`, если есть `saved_last`).
    - Иерархия страниц (`NativeMenuPage`):
      - `NATIVE_PAGE_ROOT` -> `NATIVE_PAGE_COOP_HUB` (`Создать сервер`, `Подключиться`, `Настройки коопа`, `<< Назад`)
      - `NATIVE_PAGE_HOST` (`Продолжить кооп`, `Кооп-сохранения`, `Импорт одиночного`, `Новая своя игра`, `Новая кампания`, `Настройки`, `<< Назад`)
      - `NATIVE_PAGE_JOIN` (список друзей Steam онлайн, быстрый вход по сохранённому адресу, ручной ввод IP/SteamID, вставка из буфера)
      - `NATIVE_PAGE_SETTINGS` (`Режим: 1/2 ССР`, `Казна: 100% / 50-50`, `Погранзона`, `Карта: 20x20 с границей / Вся без границ`, переключатель авто-синка модов `Моды: Общие (Авто) / Только Хоста / Из сейва`, `Проверка DLC`, `Логи Хосту: ВКЛ/ВЫКЛ`, `Язык`)
  - **Левая игровая боковая панель `☭` (`Hook_SystemTabBuild`, `RVA 0x31EAF8 -> 0x337450`):**
    - Добавляет кнопку `КООПЕРАТИВ` с советской иконкой `editor/mp_icon_coop.png` внизу системной вкладки `☭`.
    - При клике открывает подвкладку «Кооператив» (через `SYSTAB_SUBTAB_IDX_OFF = -2` и временный NOP заголовка `0x337D7B`), где отрисовывает однородные 9-slice кнопки (`DrawSidebarButton`) с квадратными иконками `96x96` (`editor/mp_icon_*.png`, `editor/settings_save.png`). Обработка клика использует `g_sidebarLClickAvailable` (объединение `INPUT_LCLICK_RVA` и переднего фронта `INPUT_LHELD_RVA` / `VK_LBUTTON`) и изолированный `PlayMenuClickSoundSafe()`, чтобы клики не терялись при активном Lockstep.
    - Содержит кнопку быстрого сохранения хоста **«Сохранить кооп-игру»** (`TwoStates_TriggerQuickCoopSave()`), перевод средств (`50 000 РУБ / $`), прыжок камеры к партнёру (`[F6]`), пинг на 20 секунд (`[Alt+ЛКМ]`), переключение 3D-границы (`[F7]`), ре-синк и остановку/отключение.
- [`lang.h`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/ui/lang.h) — Переключение языка (`MP_LANG_RU` / `MP_LANG_EN`), автоопределение по первому пункту главного меню игры и сохранение в `plugins\republicmp_settings.ini` (`[ui] lang=ru/en`).

### `session/` — Сессия, режим «Два государства», сохранения и настройки
- [`two_states.h`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/session/two_states.h) / [`two_states.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/session/two_states.cpp):
  - **Режимы:** `COOP_MODE_ONE_STATE` (0 — общая республика и казна) и `COOP_MODE_TWO_STATES` (1 — Западная ССР у Хоста `X < 0`, Восточная ССР у Клиента `X > 0`, общая погранзона `[-borderHalfWidth .. +borderHalfWidth]`).
  - **3D-граница, миникарта и сигналы (метки 20 сек):**
    - `DrawMinimapOverlay` (читает нативную миникарту по `*(BYTE**)(world + 0xD400)` при `*(BYTE*)(world + 0xFBA) == 0`) рисует поверх миникарты линию границы `X = 0`, общую погранзону, цветовую подсветку своей стороны (зелёный контур + плашка `ЗАПАД (ВЫ)` / `ВОСТОК (ВЫ)`) и стороны партнёра (красный оттенок + плашка `ЗАПАД (ПАРТНЁР)` / `ВОСТОК (ПАРТНЁР)`), а также курсор партнёра и активные метки.
    - Метки на карте (`TwoStates_PingAtCursor`, `[Alt+ЛКМ]`) отображаются в 3D и на миникарте у обоих игроков в течение **20 секунд** (`PING_LIFETIME_MS = 20000`) с обратным отсчётом в секундах.
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
    - Перед вызовом `o_masterSave(world)` создаёт снепшот полей старения техники (`+0x17E8`, `+0x1A54`, `+0x1A7C` во всех объектах `VEC_VEHICLES_BEGIN_RVA..VEC_VEHICLES_END_RVA`, которые мутируются внутри `0x6C6770 -> 0x6C1BD0`), оборачивает `o_masterSave(world)` в `Determinism_ToolScopeBegin()` / `Determinism_ToolScopeEnd()`, записывает суммарный бюджет (`g_stateRub[0] + g_stateRub[1]`) в мир, а сразу после — восстанавливает поля техники и сохраняет `republicmp_coop.ini` (`WriteCoopSaveMetaInternal`), что позволяет сохранять игру на лету во время активной Lockstep-сессии без рассинхрона.
    - Неатрибутированная дельта симуляции между хуками (`TwoStates_AfterStep`) делится пропорционально общему числу зданий и строек каждого государства (`g_stepBldCount[0] / (g_stepBldCount[0] + g_stepBldCount[1])` или `50/50`), без перекоса из-за стоимости отдельных стройплощадок.
  - **Метаданные кооп-сейва (`republicmp_coop.ini` внутри папки сохранения):**
    - Хранит `coop_mode`, `border_half_width_m`, `expand_borders`, балансы в копейках/центах (`state1_rub_cents`, `state2_rub_cents`, `state1_usd_cents`, `state2_usd_cents`), торговую статистику и списки `building_owners` / `vehicle_owners`.
    - При загрузке кооп-сейва (`ReadCoopSaveMetaInternal`) сохранённые в нём режим и бюджеты имеют **абсолютный приоритет** над настройками стартового капитала в меню.
  - **Глобальные настройки (`plugins\republicmp_settings.ini`):**
    - `TwoStates_LoadSettings()` / `TwoStates_SaveSettings()` сохраняют `mode`, `full_starting_capital`, `border_half_width_m`, `expand_borders` (по умолчанию `0` — стандартная карта 20×20 с границами), `show_3d_border`, `dlc_check`, `send_client_logs` (по умолчанию `1`), `last_join_address`, `lang`.
- [`save_sync.h`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/session/save_sync.h) / [`save_sync.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/session/save_sync.cpp) — Рукопожатие `SessionHello`, автоматическая отправка логов прошлой упавшей сессии при реконнекте (`MSG_CLIENT_LOGS = 5`), синхронизация модов и плагинов (`MSG_MOD_SYNC`), упаковка и передача мира от хоста клиенту (`mp_syncsave_host` -> `mp_syncsave_incoming`), запуск Lockstep-сессии.
- [`mod_sync.h`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/session/mod_sync.h) / [`mod_sync.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/session/mod_sync.cpp) — Полная синхронизация модов `Steam Workshop`, локальных `workshop_wip` и геймплейных плагинов `TesmioLoader`:
  - Парсит `header.bin`, `headerE.bin` (заголовок `0x260` байт, записи по `0x80` байт со строками `<WorkshopID>/<name>` и `MIRRORZ_<WorkshopID>/<name>`) для автоматического определения всех Workshop-зданий и техники, реально задействованных в сохранении.
  - Перехватывает `SOVIET_WORKSHOP_MANAGER::RefreshItems` (`RVA 0x8407C0`, указатель на менеджер `*(void**)(g_exeBase + 0x9E22B8)` = `WORLD_RVA + 0xD3A8`): в кооп-сессии фильтрует и сортирует список модов у Хоста и Клиента в идентичном порядке (`SafeFilterAndSortWorkshopMgr` через in-place swap partition с сохранением исключённых элементов в хвосте `g_hiddenTailCount` и восстановлением через `SafeRestoreWorkshopMgr`), исключая смещение индексов в Lockstep и утечки памяти.
  - Через `ModSync_BeforeWorldLoad(isHostStart, saveDir)` (вызывается из `TwoStates_BeforeAnyWorldLoad()` на главном потоке перед `o_loadWorld`) автоматически докачивает недостающие у Хоста моды из `saveDir` через `ISteamUGC`, активирует фильтр модов Хоста сразу при старте кооп-мира и безопасно выполняет `Hook_WorkshopRefresh` на главном потоке после докачки новых модов.
  - Все установленные моды включены автоматически без ручного прокликивания списков. Поддерживает 3 режима авто-синхронизации (`ModPackMode` в настройках коопа): `MOD_PACK_ALL_SHARED` (по умолчанию: общий пул всех модов Хоста, Клиента и сейва с двусторонней авто-докачкой), `MOD_PACK_HOST_ONLY` (все моды Хоста и сейва) и `MOD_PACK_SAVE_ONLY` (только моды из сейва).
  - Автоматически подписывает и докачивает недостающие моды у Хоста и Клиента через `ISteamUGC::SubscribeItem` / `DownloadItem` с отображением прогресса на экране синхронизации, а для локальных `workshop_wip` или удалённых из Steam модов автоматически передаёт архив напрямую по сети во временную песочницу `media_soviet\workshop_wip\<id>` (с маркером `.republicmp_session_temp`, очищаемым после выхода).
  - Через сервис `TSM_SERVICE_PLUGINS` в [`tesmioloader.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/src/tesmioloader.cpp) управляет горячим включением/отключением геймплейных плагинов (`resources`, `deposits`, `needs`, `depletion`, `accumulator`, `daynight`, `walking`, `cities`, `easystart`) без перезапуска игры и синхронизирует их `.ini`-конфиги во временную песочницу клиента `plugins\.mp_session_ini\`.

### `determinism/`, `lockstep/` и `world_sync/` — Детерминированная симуляция и синхронизация команд
- [`determinism.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/determinism/determinism.cpp) — Фиксированный шаг симуляции, `HookedLoadWorld` (вызывает `TwoStates_BeforeAnyWorldLoad()` до `o_loadWorld()` и `TwoStates_OnAnyWorldLoaded(session)` после), `HookedSimStep`, адаптивный тайм-пейсинг Клиента по `g_tickDebt` и глубине очереди `LockstepNet_GrantBacklog` (без рывков при пачках TCP-пакетов), отдельная монотонная фаза суб-тиковой интерполяции `g_subTickAlpha` в `Determinism_RenderAlpha()` (исключающая пилообразный джиттер на 60 Гц), сброс `g_sentPaused`/`g_sentSpeed` при старте и завершении сессии.
- [`held_frame.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/determinism/held_frame.cpp) — Отрисовка промежуточных кадров (`held frames`) без продвижения симуляции: внутрипроцессное SEH-чтение вместо `ReadProcessMemory`/`VirtualQuery` и суб-тиковая визуальная интерполяция (`60–144+ FPS`) всех 5 узлов `C3D_NODE` (`+0xD60`, `+0xE1C`, `+0xED8`, `+0xF94`, `+0x1080`, по `0xBC` байт каждый) активной техники, прицепов и вагонов (с защитой от двойной записи в `RecordOneVehicleTick` и сохранением вектора скорости в `HeldFrame_OnVehicleSoftSynced`) с полным побайтовым восстановлением состояния симуляции в `VehicleGuard::End()`.
- [`state_hash.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/determinism/state_hash.cpp) и [`vehicle_soft_sync.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/world_sync/vehicles/vehicle_soft_sync.cpp) — Разделение логического хеша техники (`hashVehicles`: ID `+0x04`, текущий шаг расписания `+0x698`, грузы `+0x3B8`), координатного хеша техники в пути (`hashVehiclesPos`: позиции техники `+0xDE0`) и хеша граждан (`hashCitizens`: дома `+0x20` и возраст `+0xD4`), а также мягкая авто-коррекция техники от Хоста к Клиенту (`LS_CMD_VEHICLE_SOFT_SYNC = 27`: топливо `+0x770`, износ `+0x17E8`/`+0x1A54`/`+0x1A7C`, скорость `+0xD24`, масса груза `+0x3B8` и плавная подтяжка всех 5 узлов `C3D_NODE` через `TranslateVehicleNodes`) без выброса ложных `DESYNC` в пути.
- [`sim_patches.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/determinism/sim_patches.cpp) — Патчи детерминизма (`HookedLate` на `0x30D1A0`, перехват оконных вызовов и видимости зданий).
- [`commands.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/commands.cpp) и [`lockstep_net.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/lockstep_net.cpp) — Очередь Lockstep-команд (с полной потокобезопасностью `g_record`/`g_replayAt`/`g_seq` под `g_commandsLock` и быстрым путём для фоновой `LS_CMD_VEHICLE_SOFT_SYNC` без сброса `g_applyEpoch` и `SettingsSync_LockstepBaseline`), обёрнутая в `TwoStates_BeginCommand` / `TwoStates_EndCommand`, окно `LOCKSTEP_WINDOW = 24`, потокобезопасная очередь `g_grants` под `g_inLock`, мягкая обработка расхождений транспорта/пешеходов в `Compare()` через многопачечный `VehicleSoftSync_RequestImmediate()`, и передача клиентских логов по каналу Lockstep (`LS_CLIENT_LOGS = 14`, `LockstepNet_SendClientLogs`).
- [`building_switches.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/building_switches.cpp) и [`vehicle_switches.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/vehicle_switches.cpp) — Синхронизация переключателей в окнах зданий, техники и нижнем меню мира (`kWorldRanges` на `0xAF8..0xB0C`, где `world + 0xB08` читается/пишется как 1 байт без захвата буфера `FormatMoney` на `world + 0xB0A`); исключают все заголовки `std::vector` и указатели внутри `BUILDING` (`kViewWords`, по данным `BUILDING::~BUILDING()` на `0x164BB0`), `SLOT` (`0x00..0x48`) и `VEHICLE` (`ListWord`), а также распознают heap-указатели в диапазоне `4 GB .. 1 TB` (`high >= 1 && high < 0x100`).
- [`settings_sync.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/session/settings_sync.cpp) и [`citizen_destinations.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/world_sync/citizens/citizen_destinations.cpp) — Синхронизация настроек зданий, слотов складов и списков назначений граждан (`0xF18`, `0xF30`, `0xF48`, `0xF60` через `CitizenDestinations_LockstepBaseline`, `CitizenDestinations_LockstepDetect` и `CitizenDestinations_ApplyLockstep`) в режиме Lockstep с проверкой прав владения зданием `TwoStates_CheckBuildingControlAllowed`.
- [`purchase.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/purchase.cpp) — Детерминированная покупка техники, б/у транспорта и чертежей/лицензий, включая реалистичную доставку категорий `cat == 1, 4, 5` через таможню и `vcat == 6, 7, 8, 10` через границу.
- [`hover_tools.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/hover_tools.cpp), [`junction_accept.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/junction_accept.cpp), [`rename.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/rename.cpp), [`cargo_settings.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/cargo_settings.cpp) и [`source_slots.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/source_slots.cpp) — Синхронизация дорожных знаков, ж/д светофоров, путевых точек, настроек перекрёстков, переименования городов/перекрёстков, настроек грузов на остановках/линиях и источников снабжения зданий с полной защитой границ и собственности в режиме «2 государства».
- [`game_calls.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/game_calls.cpp) — Перехват оконных вызовов (`LS_CMD_GAME_CALL` через 32-байтные трамплины), включая 4 кнопки приглашения жителей/специалистов в жилые дома в `0x6F8CC0` (`CALL_INVITE_CITIZENS` на `0x6FEE1E`, `0x6FF31A`, `0x6FF52D`, `0x6FFA4E` -> `0x3FFAD0`, с детерминированным спавном через `InviteCitizensNow` и `0x837EB0`).
- [`route_tools.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/route_tools.cpp) — Синхронизация маршрутных инструментов и переселения граждан (`RouteTools_WorkerRelocateTool` на `0x2B8CE0` и `ApplyWorkerRelocate` через 5-аргументный `0x83C6A0` и `0x83CA50`).
- [`brush_tools.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/lockstep/brush_tools.cpp), [`terrain_hook.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/world_sync/terrain/terrain_hook.cpp), [`local_builds.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/world_sync/local_builds.cpp), [`road_sync.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/world_sync/networks/road_sync.cpp), [`demolish_hook.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/world_sync/buildings/demolish_hook.cpp) и [`build_sync.cpp`](file:///d:/SovietRepublic%20MP/TesmioLoader/RepublicMP/world_sync/buildings/build_sync.cpp):
  - Синхронизация всех 7 методов терраформинга (`TM_RAISE_LOWER`, `TM_RAISE_LOWER_ANGLE`, `TM_SMOOTH`, `TM_SMOOTH_LIMITS`, `TM_LEVEL`, `TM_TO_HEIGHT`, `TM_MASK`) в `terrain_hook.cpp` с передачей `close = false` внутри цикла `TerrainHook_ApplyCommand` (чтобы `CloseHeightmap`/`CloseMask` не анмапили D3D11 staging-буфер на первой же операции пакета), раздельным учётом `TM_SMOOTH_LIMITS` (который сам открывает/закрывает heightmap внутри `C3DDLL64.dll`), открытием и `OpenHeightmap`, и `OpenMask` (так как `RaiseLowerTerrainAngle` меняет маску породы), и синхронной очисткой деревьев и кустарников (`0x3AE120`) при `TM_RAISE_LOWER` (выравнивание клавишей `Q` в `0x4FCAF0` и ЛКМ в `0x2BADE0`).
  - В `RelayLedger` (`brush_tools.cpp`) при перехвате `LS_CMD_LEDGER` откатывается мгновенное локальное списание `cost` из `world + 0x580` (USD) / `world + 0x588` (RUB), а в `BrushTools_ApplyLedger` стоимость списывается синхронно на обоих ПК.
  - Синхронизация железнодорожных стрелок (`RAIL`), авто-выравнивания рельефа под дорогой/тропинкой (`needsTerrainLevel` на `manager + 0x312`) и Т-образных врезок в середину сегментов для всех 14 категорий инфраструктуры (`startOwnLink` из `manager + 0x340`/`+0x348`, `endOwnLink` из `0x9E4558`/`0x9E45D8` и `0x9E4560`/`0x9E45E0`, `snapModeStart`/`snapModeEnd` на `manager + 0x74A`/`+0x74B`, `snapFlags` для `+0x5CB`/`+0x5CC` и смещения стрелок `snapOffsetStart`/`snapOffsetEnd` на `manager + 0x74C`/`+0x758`; во время вызова `0x4FCAF0` поля `+0x74A`/`+0x74B` удерживаются в `0`, чтобы избежать дублирования точек в `manager + 0x410` и `NaN`-нормалей в `0x552090`, а после `0x4FCAF0` принудительно восстанавливаются `seg.points` через `0x21D2B0` и точные координаты `startNode`/`endNode` для `ucomiss`-сравнения в `0x518A80`).
  - Полная синхронизация разрушаемости и сноса всех объектов (`v1.0.4`, протокол `41`) и полный инженерный аудит Lockstep / TwoStates (`v1.0.5`, протокол `42`):
    - Здания, стройплощадки и сносимые объекты (`0x9E6A18` и `0x9E6B38` = `world + 0x11C28`, в том числе в `Ref_Building`/`Ref_BuildingAt`, `building_switches.cpp` и `settings_sync.cpp`): сохранение и воспроизведение в `BuildSyncMsg` / `DemolishHook_CallOriginal` флагов `demolishEb1` (`building + 0xEB1`, оплата разборки деньгами), `Shift` (`0xA54BFC`, мгновенный снос / автопоиск контор), `Ctrl` (`0xA54BAC`), `BuildMode` (`world + 0xD2D4..0xD350`), подтверждения диалога `0x424` (`fromDialog424` с пропуском локального пост-блока `0x7FD000..0x7FD04E` и воспроизведением автопоиска контор сноса `0x168DC0` и назначения конторы `0x168A30`/`0x15F910` + `0x1CA9A0` на обоих ПК).
    - Инструменты назначения/отмены контор сноса (`0x1DA580`: `0x1DAAD2 -> 0x1CA9A0` и `0x1DAAE9 -> 0x15F910`), группового назначения и отмены контор (`0x1DB07D -> 0x1CC150` и `0x1DB0A1 -> 0x1CA9A0` в `0x1DAFC0`), а также переключатели приоритета объекта в окнах конторы строительства и конторы сноса (`0x7702C4` и `0x77DBB7 -> 0x1CBCE0`) в `game_calls.cpp`.
    - Инструменты удаления инфраструктуры (`terrain_demolish`, `*_cancel`, `*_destroy`, `erase_*`, с передачей `demolishType` = `manager + 0x528` и `BuildMode`), групповой снос и групповая модернизация рамкой с `Ctrl` (`0x213870`, `mode = 10` и `0x214530`, `mode = 11`), отмена планируемого сегмента по `Shift + ЛКМ` (`0x214220`, `mode = 12`), кнопка «Отменить все планируемые участки» на нижней панели (`0x39F497 -> 0x5434C0`, `exceptGroup = 2`), и точечное удаление деревьев/кустарников кликом инструмента сноса по пустой земле (`0x3ADEB6 -> 0x3AE120` и `0x3ADF12 -> 0x40EAA0`, `KIND_DEMOLISH_GROUND = 4` в `brush_tools.cpp`).

---

## 3. Справочник ключевых RVA и смещений (`SOVIET64.exe`, PE `6A3EB6AD`)

| Назначение | RVA / Смещение | Примечание |
| :--- | :--- | :--- |
| `WORLD_RVA` (`WORLD_THIS_RVA`) | `0x9D4F10` | Глобальный объект мира и главного меню (`menuObj`) |
| `WORLD_HIDE_HUD_OFF` | `world + 0xFBA` | `BYTE`: `0` — интерфейс виден, `!= 0` — интерфейс скрыт |
| `WORLD_MINIMAP_PTR_OFF` | `world + 0xD400` | Указатель на объект нативной миникарты (`+0x00` открыта, `+0x48..+0x58` геометрия экрана) |
| `SAVE_NAME_FIELD_OFFSET` | `world + 0x9F8` | `char[120]`: путь сохранения (`"save/<name>"` или `"saved_last"`) |
| `WORLD_FORCE_TERRAIN_SAVE_OFF` | `world + 0xFC90` | `BYTE`: `1` заставляет `C3D_TERRAIN::Save` записать `script.ini` и карты высот |
| `WORLD_UNLIMITED_MONEY_OFFSET` | `world + 0x578` | `int`: `!= 0` — режим неограниченных денег (`Late` на `0x30DAA2` каждый кадр сбрасывает баланс в `100000000.0`) |
| `WORLD_USD_OFFSET` / `RUB` | `world + 0x580` / `+0x588` | `double`: баланс долларов и рублей |
| `VEC_BUILDINGS_BEGIN_RVA` | `0x9E6A18` (`world + 0x11B08`) | Вектор готовых зданий (`begin = 0x9E6A18`, `end = 0x9E6A20`) |
| `VEC_CONSTRUCTIONS_BEGIN_RVA` | `0x9E6B38` (`world + 0x11C28`) | Вектор стройплощадок и сносимых зданий (`begin = 0x9E6B38`, `end = 0x9E6B40`) |
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
| `LEDGER_RVA` | `0x2FDB00` | Запись расхода в строительную книгу (`world + 0x11330` USD / `+0x11360` RUB; сайты `0x3AABCD`, `0x3AACE3`, `0x2CB854`, `0x2CB942`, `0x517244`, `0x517341`) |
| `MANAGER_RESET_RVA` | `0x542A30` | Очистка превью-состояния менеджера сети `(manager, manager + 0x408)` (удаляет `manager + 0x408` из пространственной сетки `0x56DF00`, `building + 0x7B8` и списка рендера `0x9E4A08`) |
| `RAIL_SNAP_Globals` | `0x9E4578` / `0x9E457C` | Флаг (`BYTE`) и 3D-точка (`float[3]`) привязки железнодорожной стрелки к существующему сегменту (`manager + 0x74A..0x760`) |
| `DEMOLISH_RVA` | `0x16B4C0` | Главная функция сноса здания / стройплощадки (`HookedDemolish`, сайты `0x3ADC80`, `0x6A07BF`, `0x6CDDA1`, `0x6CE3E0`, `0x7FCFFC`) |
| `CTRL_RECT_DEMOLISH_RVA` / `UPGRADE` | `0x213870` / `0x214530` | Групповой снос и групповая модернизация участков сетей и стройплощадок рамкой с `Ctrl` |
| `SHIFT_CANCEL_PLAN_RVA` | `0x214220` | Отмена планируемого участка сети по `Shift + ЛКМ` (`0x31294E` в `Late`) |
| `BOTTOM_BAR_CANCEL_CALL_RVA` | `0x39F497 -> 0x5434C0` | Кнопка «Отменить все планируемые участки» на нижней панели (`0x395C90`) |

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
13. **Обязательный сброс превью менеджера сети (`RoadSync_ClearCategoryPreview`) без обнуления `HoverGlobals` при перехвате `0x518A80` (`TakeCommitAsCommand`):**
    - В оригинальном `0x518A80` (`COMMIT_RVA`) эпилог всегда очищает превью-сегмент `manager + 0x408` (аналогично `0x542A30(manager, manager + 0x408)`), а обёртка инструмента (`0x3D00F0` и др.) сразу после возврата из `0x518A80` сбрасывает `*(BYTE*)(world + 0xD2AC) = 0` (`TOOL_DRAG_ACTIVE_OFFSET = 0`).
    - Однако `RoadSync_ClearCategoryPreview` **не должен** вызывать `HoverGlobals_Clear()`, так как при быстром клике профильного инструмента удаления (`*_cancel`, `D2C4 <= 0.2f`) обёртка делает два последовательных вызова `0x518A80` (`type = 100`, затем `0x4FCAF0(101)` + `0x518A80`), где второй вызов опирается на `0x9E4558`/`0x9E4560`.
14. **Запрет передачи `close = true` внутри пакета `TerrainHook_ApplyCommand` и подавления терраформинга при Lockstep-реплее дорог:**
    - В `C3DDLL64.dll` вызов методов терраформинга (`RaiseLowerTerrain`, `SmoothTerrain`, `SetTerrainMask` и др.) с последним аргументом `bool close = true` немедленно вызывает `CloseHeightmap(true)` / `CloseMask(true)` и анмапит D3D11 staging-буфер (`terrain + 0x628` / `+0x5B0`), из-за чего вторая и последующие операции в пакете `TerrainHook_ApplyCommand` падают с `ACCESS_VIOLATION`. Внутри `TerrainHook_ApplyEdit` всегда передавай `close = false`, а закрытие буферов выполняй один раз в конце `TerrainHook_ApplyCommand`.
    - При активном `Determinism_IsActive()` функции `ApplySegment`, `ApplyUpgrade` и `ApplyDemolishInner` **не должны** вызывать `TerrainSync_BeginForeignReplay()`, иначе выравнивание земли внутри коммита дороги подавляется на обоих ПК.
15. **Отложенный снос через диалог подтверждения `0x424` (`0x7FCFFC` в `0x7FBD40`):**
    - В `0x7FBD40` сразу после `call 0x16B4C0` (`0x7FCFFC`, возврат на `0x7FD001`) код `0x7FD001..0x7FD04E` вызывает `0x168DC0` (автопоиск контор сноса) и `0x168A30`/`0x15F910` + `0x1CA9A0` (назначение конторы из инструмента `0x1DA580` при `world + 0x117E0 == 2`). Если `HookedDemolish` перехватывает снос как Lockstep-команду и возвращает `0`, адрес возврата на стеке `*retSlot` **обязан** переводиться с `0x7FD001` на `0x7FD04F`, иначе локальный поток назначит конторы на здание, которое ещё не перешло в режим сноса, а второй ПК вообще не выполнит этот пост-блок.
