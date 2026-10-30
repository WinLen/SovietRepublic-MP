<div align="center">

# ☭ RepublicMP v1.3.1 — Кооперативный мультиплеер для Workers & Resources: Soviet Republic
### Co-op Multiplayer Mod for Workers & Resources: Soviet Republic (RU / EN)

![RepublicMP Cover](steam_workshop/preview.png)

**Полноценный кооперативный мод с детерминированной синхронизацией Lockstep, подключением через Steam P2P в 1 клик, нативным внутриигровым меню, режимом «Два государства на одной карте», автоматической синхронизацией модов Мастерской Steam и плавной 60–144+ FPS интерполяцией транспорта и камер.**

**Full-featured deterministic Lockstep co-op multiplayer mod with 1-click Steam P2P networking, native in-game menus, "Two States on One Map" mode, automatic Steam Workshop mod synchronization, and smooth 60–144+ FPS vehicle & camera interpolation.**

</div>

---

## 🇷🇺 Описание и правила установки (РУССКИЙ)

### 🔥 Ключевые возможности

#### 1. Полная интеграция в нативный интерфейс игры и кооп-штаб ожидания
- **Никаких чужеродных плавающих окон:** раздел **«КООПЕРАТИВ»** встроен прямо в главное меню игры и в левую системную панель **`☭ -> Кооператив`** со стилизованными советскими иконками.
- **Кооперативное лобби ожидания («ГОСПЛАН СССР — КООПЕРАТИВНЫЙ ШТАБ»):** автоматически открывается при создании сервера Хостом, при подключении партнёра и при внезапном обрыве связи (с кнопкой экстренного сохранения **«Сохранить кооп-игру»**). Удерживает игру на паузе и блокирует изменение мира до 100% загрузки обоих игроков. По клавише **`Tab`** сворачивается в компактный верхний баннер для свободного осмотра карты.
- **100% изоляция одиночной игры:** в одиночном режиме все хуки и оверлеи кооператива полностью неактивны.

#### 2. Два режима совместной игры
- **Одно государство (Общая республика):** классический кооператив с общим бюджетом (рубли и доллары) и совместным строительством всей республики.
- **Два государства на одной карте (Западная ССР и Восточная ССР):**
  - Карта делится на **Западную ССР (Хост, `X < 0`)** и **Восточную ССР (Клиент, `X > 0`)** с настраиваемой **общей пограничной зоной** (`150 / 250 / 500 / 1000 м`) для стыковки дорог, ЖД-путей, ЛЭП и трубопроводов.
  - **Раздельная казна и переводы:** у каждого государства свой баланс в рублях и долларах (на выбор: **100% стартового капитала каждому** или **деление 50/50**) и мгновенные переводы финансовой помощи (`50 000 РУБ / $`) из панели `☭ -> Кооператив`.
  - **Раздельная экономика, склады, демография и статистика:** счётчик населения на верхней панели HUD, окна «Экономика и торговля», «Статистика населения» и «Хранящиеся ресурсы» отображают данные исключительно вашей ССР. Полная синхронизация многоресурсных складов, стройплощадок, ползунков автозакупки и разовой ручной закупки ресурсов.
  - **Раздельный банк и кредиты:** каждое государство берёт и гасит свои кредиты в рублях и долларах с индивидуальным потолком кредитования (по взрослому населению своей республики) и раздельными ежедневными списаниями.

#### 3. Плавность 60–144+ FPS, сохранение 3D-камер и изоляция звука
- **Суб-тиковая интерполяция транспорта (`60–144+ FPS`):** плавное движение автомобилей, поездов, сцепленных вагонов, вращения и поворота колёс, привязанной камеры следования/вида из кабины и 3D-контуров выделения техники.
- **Сохранение 3D-камер каждого игрока:** позиция и точка взгляда камеры Хоста и Клиента автоматически сохраняются в кооп-сейвах (`republicmp_coop.ini`) и восстанавливаются при загрузке и ре-синке. При первом запуске новой карты или импортированного сейва камеры аккуратно выставляются в центре карты у границы с видом на свою половину республики.
- **Изоляция удалённых звуков и уведомлений:** 2D-звуки интерфейса/инструментов партнёра и уведомления с чужой территории заглушены (3D-звуки строительства/сноса партнёра слышны только в радиусе 250 м от вашей камеры).
- **Поддержка всех скоростей игры:** пауза, обычная (`>`), быстрая (`>>`) и ускоренная (`>>>` / `LCtrl+Numpad 1/2`).

#### 4. Автоматическая синхронизация модов Steam Workshop и проверка DLC
- **Два режима синхронизации модов (Host-Authoritative):**
  - **«Моды: Только Хоста (Авто)»** (по умолчанию) — синхронизируются все установленные моды Хоста и моды, прописанные в сейве.
  - **«Моды: Из сейва»** — синхронизируются только моды, реально задействованные в загружаемом сохранении.
- **Проверка ДО создания сейва:** если у Клиента не хватает модов из Мастерской Steam, открывается окно загрузки «ГОСПЛАН СССР — ЗАГРУЗКА МОДИФИКАЦИЙ» с автоматической подпиской/скачиванием и кнопкой перезапуска игры в 1 клик. Лишние моды Клиента временно скрываются на время сессии, а здания и техника из неустановленных у партнёра DLC безопасно блокируются.

#### 5. Отдельный банк кооп-сохранений (`save_coop`) и подключение через Steam P2P
- **Изолированный банк сохранений:** кооперативные сохранения (`save_coop`) полностью отделены от одиночных (`save_sp`). Доступны **«Продолжить кооп»**, быстрое сохранение из панели `☭ -> Кооператив` и безопасный **«Импорт одиночного»**.
- **Подключение в 1 клик через Steam P2P:** в меню **«КООПЕРАТИВ -> Подключиться»** отображаются друзья Steam, у которых прямо сейчас открыт кооп-сервер (`Хост: <Имя>`), без VPN и проброса портов.

---

### 🚀 Правила установки и запуска (RU)

> **Важно:** У обоих игроков должна быть одинаковая версия игры и мода **RepublicMP v1.3.1**.

#### Способ 1 — Через Steam Workshop (Рекомендуется)
1. Подпишитесь на мод в **Steam Workshop** (оба игрока).
2. Откройте папку мода (`Steam\steamapps\workshop\content\784150\3823543010\`) и **один раз** запустите **`START-COOP.bat`** — он автоматически скопирует прокси-загрузчик `dinput8.dll` и иконки меню в папку игры `SovietRepublic\` (рядом с `SOVIET64.exe`).
3. **Готово!** После этого всегда запускайте игру обычной кнопкой **«Играть»** в Steam (64-bit версия) — мод и все его будущие обновления из Steam Workshop будут загружаться автоматически.

#### Способ 2 — Архив `RepublicMP_Friend_Pack.zip` (Ручная установка)
1. Скачайте архив **`RepublicMP_Friend_Pack.zip`** из раздела [**Releases**](../../releases).
2. Распакуйте содержимое архива в корневую папку игры `SovietRepublic\` (рядом с `SOVIET64.exe`) или запустите **`START-COOP.bat`** из распакованной папки.

#### Как начать совместную игру
- **Хост:** Главное меню -> **`КООПЕРАТИВ`** -> **`Создать сервер`** -> при необходимости настройте параметры в **`Настройки`** -> выберите **`Продолжить кооп`**, **`Кооп-сохранения`**, **`Импорт одиночного`** или **`Новая своя игра`**.
- **Клиент (Игрок 2):** Дождитесь, пока Хост загрузит карту (и окажется в окне кооп-штаба ожидания) -> Главное меню -> **`КООПЕРАТИВ`** -> **`Подключиться`** -> нажмите на **`Хост: <Имя друга>`** в списке Steam (или вставьте его IP / `steam:<SteamID64>`).

### ⌨️ Горячие клавиши в игре (RU)

| Клавиша | Действие |
| :--- | :--- |
| **`☭ -> Кооператив`** | Открыть боковую панель кооператива, сохранить кооп-игру, перевести `50 000 РУБ / $` или запустить ре-синк |
| **`F6`** | Переместить камеру к 3D-курсору или сигнальному маяку партнёра |
| **`F7`** | Показать / скрыть 3D-границу между Западной и Восточной ССР |
| **`Alt + ЛКМ`** | Поставить 3D-маяк (сигнал на карте на 20 секунд) для партнёра |
| **`Tab`** | Свернуть / развернуть окно лобби ожидания для свободного осмотра карты |

---

## 🇬🇧 Description & Installation Guide (ENGLISH)

### 🔥 Key Features

#### 1. Native In-Game UI Integration & Soviet Waiting Lobby
- **No intrusive floating overlays:** the **"CO-OP"** section is integrated directly into the game's native main menu and the left-side **`☭ -> Co-op`** system tab with Soviet-styled icons.
- **Soviet Co-op Waiting Lobby ("GOSPLAN USSR — CO-OP HEADQUARTERS"):** opens automatically when hosting a server, while a partner is joining/syncing, or if your partner disconnects mid-game (providing an emergency **"Save Co-op Game"** button). Safely holds the simulation on pause and blocks world edits until both players reach 100% sync. Press **`Tab`** to collapse the lobby into a compact top banner and freely inspect the map.
- **100% Singleplayer Isolation:** all co-op hooks, patches, and overlays are completely inactive during singleplayer games.

#### 2. Two Co-op Game Modes
- **One State (Shared Republic):** classic co-op with a shared Ruble & Dollar treasury and joint construction across the entire republic.
- **Two States on One Map (Western SSR & Eastern SSR):**
  - Splits the map into **Western SSR (Host, `X < 0`)** and **Eastern SSR (Client, `X > 0`)** with a configurable **Shared Border Zone** (`150 / 250 / 500 / 1000 m`) for connecting roads, railways, power lines, and pipelines.
  - **Separate Treasuries & Wire Transfers:** independent Ruble and Dollar balances (**100% starting capital for each player** or **50/50 split**) plus instant inter-state financial transfers (`50,000 RUB / $`) via the `☭ -> Co-op` sidebar.
  - **Isolated Economy, Warehouses, Demographics & HUD Stats:** the top-bar HUD population counter, "Economy & Trade", "Population Statistics", and "Stored Resources" windows display exclusively your own republic's figures. Includes full multi-resource warehouse & construction site storage sync, auto-purchase sliders, and one-shot manual resource purchases.
  - **Separate Banking & Loans:** each state borrows and repays its own RUB and USD loans with an individual credit ceiling (calculated from your own state's adult population) and separate daily loan payments.

#### 3. Smooth 60–144+ FPS Interpolation, Saved 3D Cameras & Sound Isolation
- **Sub-tick Vehicle & Highlight Interpolation (`60–144+ FPS`):** smooth movement for road vehicles, trains, coupled wagons, wheel rotation/steering, attached follow/cabin cameras, and 3D vehicle selection/hover outlines.
- **Per-Player 3D Camera Persistence:** both Host and Client 3D camera positions and look-at targets are automatically saved in co-op saves (`republicmp_coop.ini`) and restored on load or resync. On first load of a new map or imported save, each player's camera is cleanly framed at the center border looking over their own half of the map.
- **Remote Sound & Warning Isolation:** your partner's 2D UI clicks and foreign territory notifications are muted on your screen (remote 3D construction/demolition audio plays only within 250 m of your camera).
- **Full Co-op Game Speed Support:** pause, normal (`>`), fast (`>>`), and ultra-fast (`>>>` / `LCtrl+Numpad 1/2`).

#### 4. Automatic Steam Workshop Mod Sync & DLC Guard
- **Two Host-Authoritative Mod Sync Modes:**
  - **"Mods: Host Only (Auto)"** (default) — syncs all installed Host Workshop mods plus any mods required by the save.
  - **"Mods: From Save Only"** — syncs only the Workshop mods actively used in the loaded save.
- **Pre-Save Handshake & Auto-Downloader:** Workshop mods are verified **before** packing the save. If the Client is missing mods, the in-game Gosplan modal automatically subscribes and downloads them from Steam Workshop and provides a 1-click game restart button. Extra Client-only mods are temporarily hidden for the session, and buildings/vehicles from unowned DLCs are safely guarded.

#### 5. Isolated Co-op Save Bank (`save_coop`) & 1-Click Steam P2P Join
- **Dedicated Save Bank:** co-op saves (`save_coop`) are completely separated from singleplayer saves (`save_sp`). Supports **"Continue Co-op"**, quick in-game saving from the `☭ -> Co-op` sidebar, and non-destructive **"Import Singleplayer Save"**.
- **1-Click Steam P2P Networking:** the **"CO-OP -> Join Server"** menu automatically lists online Steam friends who have an open co-op server waiting (`Host: <Friend's Name>`) — no VPN, Hamachi, or port forwarding needed.

---

### 🚀 Installation & Launch Rules (EN)

> **Important:** Both players must have the same game version and **RepublicMP v1.3.1**.

#### Method 1 — Via Steam Workshop (Recommended)
1. Subscribe to the mod on **Steam Workshop** (both players).
2. Open the Workshop mod folder (`Steam\steamapps\workshop\content\784150\3823543010\`) and run **`START-COOP.bat`** **once** — it automatically copies the `dinput8.dll` proxy loader and menu icons into your `SovietRepublic\` game folder (next to `SOVIET64.exe`).
3. **Done!** From now on, launch the game normally using the **"Play"** button in Steam (standard 64-bit version) — the mod and all future Steam Workshop updates will load automatically.

#### Method 2 — `RepublicMP_Friend_Pack.zip` Archive (Manual Install)
1. Download **`RepublicMP_Friend_Pack.zip`** from [**Releases**](../../releases).
2. Extract the archive into your `SovietRepublic\` game folder (next to `SOVIET64.exe`) or run **`START-COOP.bat`** from the extracted folder.

#### How to Start a Co-op Session
- **Host:** Main Menu -> **`CO-OP`** -> **`Host Server`** -> optionally configure **`Settings`** -> choose **`Continue Co-op`**, **`Co-op Saves`**, **`Import Singleplayer`**, or **`New Custom Game`**.
- **Client (Player 2):** Wait until the Host has loaded the map (and is in the Co-op Waiting Lobby) -> Main Menu -> **`CO-OP`** -> **`Join Server`** -> click **`Host: <Friend's Name>`** in the Steam list (or paste their IP / `steam:<SteamID64>`).

### ⌨️ In-Game Hotkeys (EN)

| Hotkey | Action |
| :--- | :--- |
| **`☭ -> Co-op`** | Open the co-op sidebar to save the co-op game, wire `50,000 RUB / $`, jump to partner, or trigger resync |
| **`F6`** | Jump camera to partner's 3D cursor or active map ping |
| **`F7`** | Show / hide the 3D border line between Western and Eastern SSR |
| **`Alt + LMB`** | Place a 20-second 3D map ping (beacon) for your partner |
| **`Tab`** | Collapse / expand the waiting lobby window to freely inspect the map |

---

## 🛠️ Сборка из исходного кода / Building from Source

Требуется Visual Studio 2022 (или Build Tools с компонентом `VC.Tools.x86.x64`):

```bat
TesmioLoader\build.bat
```

Полный прогон сборки, 15 автотестов, статического анализа (`MSVC /analyze` + `LLVM clang-tidy`) и проверки чистоты кода `ub-mode`:

```bat
TesmioLoader\run_checks.bat
```

Выходные файлы собираются в `TesmioLoader\build\` (`tesmioloader.dll`, `dinput8.dll`, `tesmiolauncher.exe`, `republicmp_tests.exe` и `plugins\RepublicMP.dll`).
