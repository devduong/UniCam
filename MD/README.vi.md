> 🌐 **Ngôn ngữ / Language:** **[🇬🇧 English](../README.md)** *([`MD/README.en.md`](README.en.md))* | **🇻🇳 Tiếng Việt**

# UniCam — Kích Hoạt Webcam MIPI CSI-2 Cho Laptop Intel Trên macOS (OpenCore EFI)

**UniCam** giúp kích hoạt webcam tích hợp MIPI CSI-2 trên nền **Intel Ice Lake IPU4P (`8086:8a19`)** trên macOS theo mô hình **100% Pure-EFI Zero-Install**:
- Toàn bộ driver nằm gọn trong phân vùng **OpenCore EFI** (`EFI/OC/ACPI/` + `EFI/OC/Kexts/`).
- **Không** cần cài bất kỳ app, service ngầm, LaunchDaemon hay extension nào vào macOS.
- Hoạt động trực tiếp trên **Photo Booth**, **FaceTime**, **QuickTime Player**... qua `UVCAssistant` gốc của Apple, tự động bật/tắt **đèn trắng Privacy LED** và phục hồi ổn định sau **Sleep/Wake** (S3/S0ix).
- Hỗ trợ nhiều cảm biến OmniVision (**`OV01A10`**, **`OV01A1B`**, **`OV2740`**, **`OV02C10`**, **`OV5693`**, **`OV5675`**) và cả 2 kiểu mạch nguồn phổ biến (**Discrete GPIO `INT346F/INT3472`** và **TI `TPS68470` I2C PMIC `UNIC0002`**).

> [!NOTE]
> **Đã kiểm chứng thực tế trên phần cứng:** **Dell XPS 13 7390 2-in-1** (`Intel Ice Lake IPU4P 8086:8a19` + `OV01A10`, Port 6 1-lane, macOS 26.7.2 Tahoe, OpenCore `1.0.9`) — thu hình RAW10/SoftISP trực tiếp và lên hình mượt trên **Photo Booth**. Các tổ hợp **IPU4P `8086:8a19`** khác *(như `OV5693 + TPS68470` trên Surface Pro 7, hoặc `OV2740`/`OV02C10`/`OV5675`)* đã được tích hợp sẵn driver từ mã nguồn Linux mainline và **cần người dùng có máy thật chạy thử xác nhận** (xem mục 3 bên dưới).

---

## 1. Yêu Cầu Hệ Điều Hành, Kext Phụ Thuộc & Thành Phần Trên EFI

### 1.1. Phiên bản macOS hỗ trợ (`x86_64`)

| Phiên bản macOS | Trạng thái | Ghi chú kỹ thuật |
| :--- | :--- | :--- |
| **macOS 26 Tahoe** *(vd `26.7.2`, build `25G332`)* | **Đã kiểm chứng thực tế** | Biên dịch và chạy thực tế ổn định với `UVCAssistant` (Photo Booth + Privacy LED + Sleep/Wake). |
| **macOS 15 Sequoia / 14 Sonoma / 13 Ventura** | **Hỗ trợ bằng cùng 1 bản build** *(cần xác nhận)* | Kext biên dịch với `-mmacosx-version-min=13.0` (`LSMinimumSystemVersion = 13.0`), dùng chung **1 bản `UniCam.kext` duy nhất** tải tại [**Releases**](https://github.com/devduong/UniCam/releases) *(chỉ cần `Lilu.kext` & `VoodooI2C.kext` khớp bản macOS của bạn)*. |
| **macOS mới hơn / Beta (`Darwin >= 26`)** | **Khóa an toàn (cần `-unicambeta`)** | Mặc định từ chối nạp trên nhân Darwin mới hơn bản đã test để tránh panic ABI `IOUSBHostFamily`; thêm cờ `-unicambeta` vào `boot-args` nếu muốn test thử. |
| **macOS 12 Monterey trở xuống** *(12 / 11 / 10.15)* | **Không hỗ trợ** | Hệ thống cũ dùng `VDCAssistant` và kiến trúc USB video stack khác `UVCAssistant` của macOS 13+. |

### 1.2. Các kext phụ thuộc, phiên bản & thứ tự nạp trong `EFI/OC/`

Để webcam hoạt động, **chỉ cần các thành phần sau nằm trong phân vùng OpenCore EFI (`EFI/OC/`)**, nạp đúng thứ tự trong `config.plist`:

| Thành phần & Bundle ID | Phiên bản tối thiểu / Khuyến nghị | Đã kiểm chứng trên máy thật | Thứ tự & Vai trò |
| :--- | :--- | :--- | :--- |
| **OpenCore Bootloader** | `>= 0.9.5` *(`>= 1.0.2` cho Sequoia/Tahoe)* | **`1.0.9`** | Nạp ACPI SSDT & kext từ EFI mà không cần sửa macOS (`Pure-EFI`). |
| **`Lilu.kext`** *(cài sẵn trên EFI)*<br>`as.vit9696.Lilu` | `>= 1.6.7` *(`>= 1.7.0` cho macOS 15/26)* | **`1.7.2`** | **`Kernel -> Add` #1 (đầu tiên):** Kext nền tảng Hackintosh. |
| **`VoodooI2CServices.kext`** *(plugin trong `VoodooI2C.kext`)*<br>`com.alexandred.VoodooI2CServices` | Đi kèm gói `VoodooI2C` | **`1.0`** *(build `1`)* | **`Kernel -> Add` trước `VoodooI2C`:** Cung cấp tầng dịch vụ I2C nội bộ cho `VoodooI2C`. |
| **`VoodooGPIO.kext`** *(plugin trong `VoodooI2C.kext`)*<br>`org.coolstar.VoodooGPIO` | Đi kèm gói `VoodooI2C` | **`1.1`** | **`Kernel -> Add` trước `VoodooI2C`:** Driver điều khiển bộ GPIO PCH Intel (`INT3455`). |
| **`VoodooI2C.kext`**<br>`com.alexandred.VoodooI2C` | ABI `>= 2.0`, khuyến nghị `>= 2.8` *(`>= 2.9.x` cho macOS 15/26)* | **`2.9.1`** | **`Kernel -> Add` ngay trước `UniCam.kext`:** Bắt buộc cung cấp `VoodooI2CDeviceNub` cho cảm biến (`UNIC0001`) và PMIC (`UNIC0002`). |
| **`UniCam.kext`** *(của dự án này)*<br>`com.unicam.UniCam` | **`1.0.0`** | **`1.0.0`** | **`Kernel -> Add` SAU `VoodooI2C.kext`:** Kext hợp nhất điều khiển nguồn/I2C, **đã nhúng sẵn firmware IPU4P**, chạy SoftISP Q10 và giả lập camera UVC chuẩn Apple. |
| **`SSDT-CAM-<May>.aml`** | Theo dòng máy | `SSDT-CAM-Dell7390.aml` | **`EFI/OC/ACPI/` (`ACPI -> Add`):** Sơ đồ chân GPIO, `MCLK`, bus I2C và cổng MIPI CSI-2 riêng của máy (tạo bằng [`UniCam-SSDT.command`](../acpi/UniCam-SSDT.command)). |

- **Trong BIOS:** bật mục *Camera / Integrated Webcam* (**Enabled**).
- **Trong macOS:** **không cần cài thêm bất cứ file, app hay LaunchDaemon nào** (firmware IPU4P đã được nén `zlib` sẵn vào bên trong `UniCam.kext`).
- *(Phụ thuộc hệ thống Apple sẵn có trong `Info.plist` — không cần tải thêm: `IOACPIFamily >= 1.4`, `IOPCIFamily >= 2.9`, `IOUSBHostFamily >= 1.2`, `AppleUSBCommon >= 1.0`, KPI `14+`.)*

---

## 2. Hướng Dẫn Nhanh Cho Người Không Biết Lập Trình

Dự án được chia gọn thành **2 thư mục tự chứa**, mỗi thư mục có sẵn file **nhấp đúp chuột** trên Finder:
- [`kexts/`](../kexts/README.md): Chứa toàn bộ mã nguồn, firmware, cấu hình cảm biến và trình build 1-click **[`Build-UniCam.command`](../kexts/Build-UniCam.command)**.
- [`acpi/`](../acpi/README.md): Chứa [`SSDT-CAM-DUMP.aml`](../acpi/SSDT-CAM-DUMP.aml) + trình hướng dẫn tự động **[`UniCam-SSDT.command`](../acpi/UniCam-SSDT.command)** để dò chân và tự tạo file `SSDT-CAM-<May>.aml`.

### Bước 1 — Tải sẵn `UniCam.kext` tại Releases *(hoặc tự build)*
- **Cách 1 — Khuyên dùng (không cần cài Xcode):**
  1. Vào trang **[GitHub Releases](https://github.com/devduong/UniCam/releases)**, tải file `.zip` phát hành mới nhất.
  2. Giải nén để lấy sẵn **`UniCam.kext`** *(1 bản dùng chung từ **macOS 13 Ventura → macOS 26 Tahoe**, đã nhúng sẵn firmware Intel IPU4P bên trong)*.
- **Cách 2 — Nếu bạn muốn tự biên dịch từ mã nguồn:**
  1. Mở thư mục [`kexts/`](../kexts/README.md), nhấp đúp **[`Build-UniCam.command`](../kexts/Build-UniCam.command)** *(nếu macOS hỏi Gatekeeper: chuột phải -> **Open** -> **Open**)*.
  2. Nếu máy chưa có công cụ biên dịch của Apple, cửa sổ *Xcode Command Line Tools* sẽ bật lên -> bấm **Install** rồi chạy lại `Build-UniCam.command`.
  3. Script tự tải firmware chính thức từ Intel/Microsoft, nhúng vào kext và xuất ra **`kexts/UniCam/build/UniCam.kext`**.

### Bước 2 — Chuẩn bị file `SSDT-CAM-<May>.aml` cho dòng máy của bạn

> [!IMPORTANT]
> **Chỉ chọn ĐÚNG 1 trong 3 cách dưới đây** phù hợp với máy bạn *(không cần làm cả 3)*:

- **Tùy chọn 1 — Máy đã có sẵn file biên dịch (Dell XPS 13 7390 2-in-1):**
  - Lấy thẳng file **[`SSDT-CAM-Dell7390.aml`](../acpi/boards/dell-xps-7390-2in1/SSDT-CAM-Dell7390.aml)** *(không cần dò chân, không cần biên dịch)* rồi chuyển ngay sang **Bước 3**. *(Hoặc mở [`UniCam-SSDT.command`](../acpi/UniCam-SSDT.command) -> chọn **`[3]`** để chép ra `acpi/output/`).*

- **Tùy chọn 2 — Dùng công cụ tự động [`UniCam-SSDT.command`](../acpi/UniCam-SSDT.command) *(Khuyên dùng cho máy IPU4P mới, không cần biết code)*:**
  - *Điều kiện trước khi chạy:* Có [`MaciASL.app`](https://github.com/acidanthera/MaciASL/releases) trong `/Applications` *(hoặc cài `brew install acpica`)* để tool tự gọi `iasl` biên dịch `.aml`.
  - Nhấp đúp **[`acpi/UniCam-SSDT.command`](../acpi/UniCam-SSDT.command)** *(nếu macOS hỏi Gatekeeper: chuột phải -> **Open** -> **Open**)*.
  - **Nếu bạn chưa biết sơ đồ chân BIOS** *(cần dò tự động qua 2 lần mở tool)*:
    1. *Lần 1:* Chọn **`[1]`** để xuất **`acpi/output/SSDT-CAM-DUMP.aml`** -> chép vào `EFI/OC/ACPI/`, bật trong `config.plist` rồi **Reboot** macOS.
    2. *Lần 2:* Mở lại `UniCam-SSDT.command`, chọn **`[2]`** -> tool tự đọc sơ đồ chân BIOS vừa dump, cho bạn chọn đúng mã cảm biến và biên dịch ra **`acpi/output/SSDT-CAM-<TenMay>.aml`**.
    3. Mở `config.plist` **TẮT (`Enabled = False`) `SSDT-CAM-DUMP.aml`** (chỉ giữ lại `SSDT-CAM-<TenMay>.aml`).
  - **Nếu bạn đã biết sẵn thông số chân** *(từ DSDT/Windows/Linux)*: Chọn thẳng **`[4]`** trong `UniCam-SSDT.command` để nhập thông số ngay trên menu và xuất trực tiếp `.aml` *(không cần bước `SSDT-CAM-DUMP`)*.

- **Tùy chọn 3 — Tạo thủ công *(Không dùng `UniCam-SSDT.command`, dành cho người quen MaciASL hoặc Terminal)*:**
  - **Cách A — Chỉnh tay bằng `MaciASL.app`:**
    1. Mở [`acpi/templates/SSDT-CAM-Static.dsl`](../acpi/templates/SSDT-CAM-Static.dsl) *(điền địa chỉ tĩnh)* hoặc [`acpi/templates/SSDT-CAM-IntelGNVS.dsl`](../acpi/templates/SSDT-CAM-IntelGNVS.dsl) *(đọc động biến GNVS BIOS)* bằng `MaciASL.app`.
    2. Sửa đúng `Scope (\_SB.PCI0.I2Cx)` *(hoặc `PC00`)*, địa chỉ I2C (`I2cSerialBusV2`), mã cảm biến (`unicam-sensor-hid` / `unicam-sensor-compat`), cổng & số làn MIPI CSI-2, `MCLK`, và địa chỉ GPIO DW0 / PMIC `TPS68470` của máy.
    3. Chọn **File -> Save As -> File Format: ACPI Machine Language Binary (`.aml`)** để lưu thành `SSDT-CAM-<TenMay>.aml`.
  - **Cách B — Tạo qua Terminal CLI:** Xem hướng dẫn lệnh `parse_cam_dump.py` và `gen_board_ssdt.py` ở cuối file [`acpi/README.md`](../acpi/README.md).

### Bước 3 — Bỏ vào OpenCore EFI và Mở Photo Booth
1. Chép **`SSDT-CAM-<TenMay>.aml`** vào `EFI/OC/ACPI/` và **`UniCam.kext`** vào `EFI/OC/Kexts/`.
2. Mở `EFI/OC/config.plist` (bằng ProperTree hoặc OpenCore Configurator):
   - **`ACPI -> Add`**: Thêm & bật `SSDT-CAM-<TenMay>.aml` (tắt `SSDT-CAM-DUMP.aml`).
   - **`Kernel -> Add`**: Bảo đảm thứ tự nạp là **`Lilu.kext`** → **`VoodooI2C.kext`** *(+ plugins)* → **`UniCam.kext`**.
3. **Reboot** macOS, mở **Photo Booth** (hoặc QuickTime / FaceTime) và kiểm tra camera **Integrated Webcam HD**.

#### Các cờ `boot-args` OpenCore khi test & phòng ngừa sự cố

Thêm các cờ cần thiết vào `config.plist` → `NVRAM -> Add -> 7C436110-AB2A-4BBB-A880-FE41995C9F82 -> boot-args`:

| Cờ `boot-args` | Tác dụng | Khi nào nên thêm |
| :--- | :--- | :--- |
| *(không thêm — mặc định)* | Chỉ gắn Virtual UVC và cấp nguồn camera khi máy có đủ Ice Lake `IPU4P (8086:8a19)` + `SSDT-CAM` + cảm biến được hỗ trợ trên macOS 13–26; tự nằm yên nếu phần cứng không khớp. | Dùng hằng ngày ổn định. |
| **`-unicamdbg`** | Bật log debug chi tiết, cho phép gắn Virtual UVC dự phòng (bảng màu chẩn đoán) trên máy `IPU4P` đang dò SSDT, và cho phép probe thủ công qua CLI trên cảm biến mới. | **Khuyên dùng khi test dòng máy mới ở mục 3.2** hoặc chẩn đoán lỗi đen hình. |
| **`-unicamtest`** | Ép bật Virtual UVC giả lập (`Integrated Webcam HD` xuất bảng màu test) kể cả trên máy không có `IPU4P` hoặc chưa có `SSDT-CAM`. | Test riêng tầng `UVCAssistant` / ứng dụng gọi webcam mà không cần phần cứng thật. |
| **`-unicambeta`** | Bỏ chốt chặn phiên bản Darwin để cho phép nạp `UniCam.kext` trên các bản macOS mới hơn macOS 26 Tahoe (`Darwin >= 26`). | Khi muốn test trên bản macOS beta/mới hơn macOS 26. |
| **`-unicamoff`** | Vô hiệu hóa hoàn toàn mọi dịch vụ của `UniCam.kext` ngay trong `probe()`/`start()`. | Công tắc tắt khẩn cấp khi boot gặp sự cố. |

---

## 3. Chi Tiết Các Dòng Máy Intel IPU4P: Đã Hoạt Động vs Cần Xác Nhận

### 3.1. Đã Hoạt Động Thực Tế (Verified Working)

| Dòng máy cụ thể | CPU / IPU PCI ID | Mạch nguồn / Clock | Cảm biến (ACPI HID) | Kênh MIPI | Trạng thái & Hồ sơ EFI |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Dell XPS 13 7390 2-in-1** *(bản xoay gập 2-in-1, có IPU4P)* | Core Gen 10 Ice Lake<br>**IPU4P `8086:8a19`** | Discrete GPIO (`INT346F`, `C1TP=1`) + PCH `IMGCLKOUT_1` (`19.2 MHz`) + Privacy LED | **OmniVision `OV01A10`**<br>(`OVTI01A0`, `1280x800`) | **Combo PHY Port 6**, `1-lane`, `400 MHz` | **Đã chạy thực tế** (Photo Booth + Privacy LED + Sleep/Wake). Dùng ngay [`SSDT-CAM-Dell7390.aml`](../acpi/boards/dell-xps-7390-2in1/SSDT-CAM-Dell7390.aml). |

---

### 3.2. Đã Hỗ Trợ Sẵn Trong Kext & ACPI — Cần Máy Thật Xác Nhận (Supported, Pending Confirmation)

Kext và bộ công cụ ACPI **đã hỗ trợ đầy đủ** toàn bộ phần mềm cần thiết cho nền **Intel Ice Lake `IPU4P` (`8086:8a19`)** ở cả 2 chuẩn PHY (**D-PHY** Port `3`, `8..12` lẫn **Combo PHY** Port `6`, `7`), cả 2 kiểu mạch nguồn (**Discrete GPIO** lẫn **TI `TPS68470` I2C PMIC `UNIC0002`**), cùng **SoftISP nâng trần lên `1936x1096`** cho 6 cảm biến OmniVision dưới đây. Tuy nhiên, nhóm tác giả **chưa có sẵn máy thật ngoài Dell XPS 7390 2-in-1** để tự test nên cần cộng đồng xác nhận:

| Tổ hợp phần cứng IPU4P (`8086:8a19`) | Ví dụ dòng laptop cụ thể thường gặp | Mạch nguồn / Cảm biến đã viết sẵn trong kext | Điểm cần cộng đồng có máy thật xác nhận |
| :--- | :--- | :--- | :--- |
| **IPU4P + `TPS68470 PMIC` (`INT3472`) + `OV5693`** | **Microsoft Surface Pro 7** *(camera trước)*, **Microsoft Surface Book 3** *(13.5" / 15" Ice Lake)* | [`Tps68470PowerHal`](../kexts/UniCam/src/Core/BoardHal/Tps68470PowerHal.cpp) (`UNIC0002`) + [`OV5693Driver`](../kexts/UniCam/src/Core/Sensors/OV5693Driver.cpp) (`INT33BE`, `1296x972` 2x2 bin).<br>*(Camera sau `OV8865` / IR `OV7251` hiện chưa hỗ trợ)* | Xác nhận bus/địa chỉ I2C của `TPS68470`, ánh xạ chân GPIO reset/pwdn trên PMIC, số làn MIPI/Port và khả năng bắt frame trên cổng D-PHY/Combo PHY. |
| **IPU4P + Discrete GPIO / `TPS68470` + `OV01A10` / `OV01A1B`** | Các biến thể **Dell XPS / Latitude Ice Lake 2-in-1** (cùng họ `8086:8a19`), hoặc máy **HP / Lenovo Ice Lake-U** bản camera HD MIPI | [`OV01A10Driver`](../kexts/UniCam/src/Core/Sensors/OV01A10Driver.cpp) (`OVTI01A0` / `OVTI01AB`, `1280x800`) | Chạy `UniCam-SSDT.command` để lấy đúng `SSDT-CAM-<May>.aml` theo sơ đồ chân BIOS, xác nhận góc xoay `0`/`180` và IPU port mapping. |
| **IPU4P + Discrete GPIO / `TPS68470` + `OV2740` / `OV02C10`** | Một số cấu hình MIPI (không phải USB UVC) trên **Lenovo Yoga C940-14IIL**, **HP Spectre x360 13-aw0xxx**, **Dell Latitude 9410 / 9510 2-in-1** *(bản Ice Lake)* | [`OV2740Driver`](../kexts/UniCam/src/Core/Sensors/OV2740Driver.cpp) (`INT3474`/`OVTI2740`, `1932x1092`) + [`OV02C10Driver`](../kexts/UniCam/src/Core/Sensors/OV02C10Driver.cpp) (`OVTI02C1`, `1928x1092`, 1 hoặc 2 lane) | Xác nhận link frequency (`360`/`180` hay `400 MHz`), số làn CSI (`1` hay `2` lanes), cân bằng trắng Bayer (`SGRBG10`) và chất lượng ảnh SoftISP khi crop/scale về `1280x720`. |
| **IPU4P + Discrete GPIO / `TPS68470` + `OV5675`** | Laptop mỏng nhẹ Intel Ice Lake dùng cảm biến 5MP OmniVision (`OVTI5675`/`INT3476`) chạy mode 2x2 binned | [`OV5675Driver`](../kexts/UniCam/src/Core/Sensors/OV5675Driver.cpp) (`OVTI5675`/`INT3476`, `1296x972`, 900 Mbps 2-lane) | Xác nhận khả năng I2C probe ID `0x5675`, đồng bộ PHY `450 MHz` trên IPU4P và màu sắc Bayer `SGRBG10`. |

> [!TIP]
> **Nếu bạn đang sở hữu một trong các máy thuộc nhóm 3.2 muốn test thử & gửi xác nhận:**
> 1. Kiểm tra nhanh máy có chip **Intel IPU4P (`8086:8a19`)** không (xem Device Manager trên Windows -> `PCI\VEN_8086&DEV_8A19`, hoặc `ioreg -l | grep 8a19` trên macOS).
> 2. Làm theo Bước 1 & Bước 2 ở trên (dùng [`Build-UniCam.command`](../kexts/Build-UniCam.command) và [`UniCam-SSDT.command`](../acpi/UniCam-SSDT.command)), đồng thời thêm **`-unicamdbg`** vào OpenCore `boot-args` *(thêm cả **`-unicambeta`** nếu đang test trên `Darwin >= 26`)* để bật log chẩn đoán và giữ chế độ UVC dự phòng khi cấu hình máy mới.
> 3. Nếu webcam chưa lên hình ngay, mở Terminal chạy 2 lệnh sau (khi đã bật boot-arg `-unicamdbg`) rồi mở GitHub Issue đính kèm file `acpi/output/pinout_decoded.json`:
>    `ioreg -r -n UniCamBridgeService -d 4 && log show --last 5m --predicate 'eventMessage CONTAINS[c] "UniCam"'`

---

### 3.3. Các Dòng Máy Không Cần Hoặc Chưa Hỗ Trợ

- **Webcam USB UVC truyền thống *(không cần UniCam)*:** Nhiều laptop Intel Gen 10 (kể cả **Dell XPS 13 7390 bản thường không phải 2-in-1**, các dòng **Comet Lake Gen 10**, hoặc máy có webcam gắn qua bus USB `XHC`) đã nhận webcam gốc trên macOS hoặc chỉ cần `USBMap` — **không dùng Intel IPU4P và không cần cài project này**.
- **Intel IPU3 *(Skylake / Kaby Lake / Amber Lake — Gen 7/8/9, ví dụ Surface Go 1/2/3, Dell Latitude 5285, Latitude 7212)*:** Khác thế hệ IPU (`8086:1919` / `8086:5919`), chưa hỗ trợ HAL `IPU3`.
- **Intel IPU6 / IPU6EP / IPU6SE *(Tiger Lake / Alder Lake / Raptor Lake — Gen 11/12/13/14, ví dụ Dell XPS 13 9310/9315/9320, Surface Pro 7+/8/9)*:** Khác kiến trúc `IIpuIsysHal` và firmware (`8086:9a19` / `8086:465d` / `8086:a75d`), hiện chưa hỗ trợ.

### 3.4. Mở Rộng Cho Nhà Phát Triển (Developer Guide)

| Thành phần muốn mở rộng | Điểm mở rộng trong mã nguồn |
| :--- | :--- |
| **Thêm cảm biến MIPI mới** (`OV8865`, `OV7251`, `HI556`, `HM2170`...) | Kế thừa [`ISensorDriver.hpp`](../kexts/UniCam/src/Core/Sensors/ISensorDriver.hpp), đăng ký `_HID`/`compat` trong [`SensorRegistry.cpp`](../kexts/UniCam/src/Core/Sensors/SensorRegistry.cpp) và thêm file nguồn vào [`kexts/UniCam/Makefile`](../kexts/UniCam/Makefile). |
| **Thêm PMIC nguồn I2C khác** | Thêm backend kế thừa [`IBoardPowerHal.hpp`](../kexts/UniCam/src/Core/BoardHal/IBoardPowerHal.hpp) (tham khảo [`Tps68470PowerHal.cpp`](../kexts/UniCam/src/Core/BoardHal/Tps68470PowerHal.cpp)). |
| **Thêm thế hệ IPU mới (`IPU3`/`IPU6`)** | Triển khai giao diện [`IIpuIsysHal.hpp`](../kexts/UniCam/src/IPU/Hal/IIpuIsysHal.hpp) và bổ sung PCI ID trong [`Info.plist`](../kexts/UniCam/Info.plist). |

```bash
# Biên dịch toàn bộ và kiểm tra tính toàn vẹn (ACPI Dell 7390 + UniCam.kext)
make clean && make verify

# Kiểm tra dịch vụ IORegistry đang nạp & nhật ký debug kernel (-unicamdbg)
ioreg -r -n UniCamBridgeService -d 4
log show --last 5m --predicate 'eventMessage CONTAINS[c] "UniCam"'
```

---

## 4. Cấu Trúc Thư Mục Dự Án

- **[`kexts/`](../kexts/README.md)** — Độc lập 100% thành phần kext: `Build-UniCam.command` + [`UniCam/`](../kexts/UniCam/Makefile) (`src/`, `include/`, `firmware/`, `profiles/sensors/`, `scripts/`).
- **[`acpi/`](../acpi/README.md)** — Độc lập 100% thành phần ACPI: `UniCam-SSDT.command` + `SSDT-CAM-DUMP.aml` + [`scripts/`](../acpi/scripts/unicam_ssdt_wizard.py) + [`templates/`](../acpi/templates/SSDT-CAM-Static.dsl) + [`boards/`](../acpi/boards/dell-xps-7390-2in1/SSDT-CAM-Dell7390.dsl).
- **[`MD/`](README.en.md)** — Tài liệu đa ngôn ngữ ([`README.en.md`](README.en.md), [`README.vi.md`](README.vi.md)).
- **[`Makefile`](../Makefile)** — Các target biên dịch và kiểm tra ở thư mục gốc (`make clean && make verify`).
