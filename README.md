# TempModFilter — HWID Spoofer (مختبر)

> أداة تغيير هوية الجهاز (HWID Spoofer) مؤقتة في الذاكرة (RAM) + Registry لـ Windows.
> **للاستخدام في المختبر / VM فقط.** لا تقم بتثبيته على مضيف إنتاج.

TempModFilter هو فلتر علوي (upper filter) بوضع Kernel (KMDF) مرتبط بفئة إعداد `DiskDrive`.
يعترض عمليات الإدخال/الإخراج في طريق عودتها إلى وضع المستخدم (User Mode) ويقوم بإعادة كتابة
أنماط بايتات (byte patterns) مختارة داخل المخازن (buffers) المُعادة **في RAM فقط**.
لا يُكتب أي شيء إلى القرص، أو firmware، أو SPI flash، أو NVRAM، أو EEPROM.
إعادة التشغيل (Reboot) تستعيد قيم القرص الأصلية بالكامل (القيم في Registry تبقى حتى `restore`).

تم تجميعه الآن كـ **مجموعة أدوات HWID spoofer**:

- تغيير رقم التسلسلي للقرص (Disk serial) وطرازه و firmware مؤقتًا (في الذاكرة فقط) عبر فلتر Kernel.
- تغيير قيم Registry: MachineGuid، ProductId، Hostname، HwProfileGuid، SQM MachineId، InstallDate.
- تغيير عنوان MAC لمحولات الشبكة عبر Registry.
- عميل وضع المستخدم (`FilterClient.exe`) مع أوامر لكل خطوة + `show` / `restore` / `all`.

---

## الميزات

| المجال | القدرة |
|--------|--------|
| رقم تسلسلي للقرص | استبدال serial/model/firmware/vendor في مخازن IOCTL (في RAM) |
| تغيير عشوائي | توليد قيم عشوائية بنفس الطول |
| جميع الأقراص | `--all-drives` يطبق على كل PhysicalDrive |
| عنوان MAC | ضبط `NetworkAddress` لمحول شبكة (أو كل المحولات) عبر Registry |
| MachineGuid | ضبط `HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid` |
| ProductId | ضبط `ProductId` لـ Windows |
| Hostname | ضبط اسم الحاسوب / NetBIOS / TCP hostname |
| HwProfileGuid | ضبط hardware profile GUID |
| SQM MachineId | ضبط `SOFTWARE\Microsoft\SQMClient\MachineId` |
| InstallDate | ضبط `InstallDate` |
| الكل في واحد | `spoof all` ينفّذ كل الخطوات |
| محرك إعادة التسمية | استبدال سلسلة بايتات (ليس بايتًا واحدًا) |
| pass-through | مسح السياسة (policy) يعيد القيم الأصلية فورًا |
| عدادات | الطلبات المُشاهدة / المخازن المُفحوصة / إجمالي الاستبدالات |
| نسخ احتياطي | `restore` يعيد قيم Registry الأصلية من نسخة احتياطية |

لا شيء يثبت (persist) لمسار القرص: مسح السياسة أو إعادة التشغيل يعيد القيمة الأصلية.
قيم Registry/MAC تبقى حتى تشغيل `restore` (أو استعادة يدوية).

---

## أوامر العميل (Client commands)

```
FilterClient.exe status                      عرض السياسة (policy) المُفعّلة
FilterClient.exe stats [--reset]             عرض / إعادة تعيين العدادات
FilterClient.exe set --pattern <p> [opts]    تفعيل قاعدة إعادة تسمية واحدة
        --replacement <str>   سلسلة استبدال
        --replace 0x2A        بايت يُكتب فوق كل تطابق
        --ioctl   0x2D1400    IOCTL مستهدف (0 = الكل)
        --max     8           استبدالات لكل طلب
FilterClient.exe clear                       إلغاء التفعيل (pass-through)
FilterClient.exe demo [--drive 0]            قبل/بعد round-trip على قرص
FilterClient.exe show                        عرض هوية الجهاز الحالية
FilterClient.exe spoof disk ...              تغيير هوية القرص في RAM (volatile)
        [--drive 0] [--all-drives] [--random]
        [--serial <s>] [--model <m>] [--firmware <f>] [--vendor <v>]
        [--all-fields]
FilterClient.exe spoof mac ...               ضبط MAC للAdapter عبر Registry
        [--address <mac>] [--index 0|--all] [--random]
FilterClient.exe spoof guid [--value <g>]    ضبط MachineGuid (عشوائي إذا أُهمل)
FilterClient.exe spoof productid [--value <p>]  ضبط Windows ProductId
FilterClient.exe spoof hostname --value <n>  ضبط اسم الحاسوب
FilterClient.exe spoof hwprofile [--value <g>]  ضبط HwProfileGuid
FilterClient.exe spoof sqm                   ضبط SQM MachineId
FilterClient.exe spoof installdate [--value <n>]  ضبط InstallDate
FilterClient.exe spoof all                   تنفيذ كل خطوات spoof (عشوائي)
FilterClient.exe restore                    مسح الفلتر + استعادة Registry
FilterClient.exe wmi show                   استعلام WMI عن قيم HWID
FilterClient.exe wmi backup                 نسخ احتياطي لمستودع WMI
FilterClient.exe wmi spoof --from <a> --to <b>  استبدال binary في مستودع WMI (طول متساوٍ)
FilterClient.exe wmi spoof --reset-repository  إعادة تعيين مستودع WMI
FilterClient.exe simulate ...               تشغيل خوارزمية إعادة التسمية في user mode
FilterClient.exe help
```

بنية النمط (Pattern): `"ABC123"` أو `"hex:41 42 43"`.

---


### 
One-click run
 `Spoofer-Setup.cmd` / `Spoofer-Run.cmd`

- `Spoofer-Setup.cmd`: double-click -> admin -> test signing + sign driver + install -> reboot prompt.
- `Spoofer-Run.cmd`: after reboot, double-click -> numbered menu (status / show / backup / spoof all / spoof disk / wmi / restore / demo).

## البناء (Build)

المتطلبات: Windows 10/11 x64، Visual Studio 2022 مع "Desktop development with C++"،
Windows Driver Kit (WDK) + Windows SDK مطابق.

```powershell
git clone https://github.com/GitGlab2/TempModFilter.git
cd TempModFilter
.\scripts\Build.ps1                     # Debug|x64
.\scripts\Build.ps1 -Configuration Release
```

مخرجات البناء:
```
build\Debug\x64\driver\TempModFilter.sys
build\Debug\x64\user\FilterClient.exe
```

يمكن بناء عميل وضع المستخدم (User-mode client) بشكل مستقل باستخدام MSVC + Windows SDK (بدون WDK).

---

## توقيع الاختبار (Test signing)

```powershell
.\scripts\Sign-Test.ps1
```

على VM:
```cmd
bcdedit /set testsigning on
```

(يجب تعطيل Secure Boot.)

---

## التثبيت (Install)

```powershell
.\scripts\Install.ps1
```

ثم أعد التشغيل (Reboot) ليتم ربط الفلتر بفئة الأقراص (disk class).

## إلغاء التثبيت (Uninstall)

```powershell
.\scripts\Uninstall.ps1
.\scripts\Uninstall.ps1 -RestoreBackup -Reboot
```

---

## اختبار Round-trip

```powershell
.\scripts\Test-RoundTrip.ps1
```

يثبت أن الرقم التسلسلي يُخفى أثناء التفعيل ويُستعاد بعد الإلغاء.

---

## البنية (Architecture)

```
User mode
   |  DeviceIoControl(IOCTL_STORAGE_QUERY_PROPERTY)
   v
I/O Manager  ->  IRP (METHOD_BUFFERED)
   |
   v
[ TempModFilter.sys ]  Upper filter (EvtIoDefault)
   |     1. WdfRequestFormatRequestUsingCurrentType
   |     2. WdfRequestSend(lower target) + completion routine
   v
[ disk.sys / storport ]  ->  hardware
   |
   v
completion routine (قد يعمل على DISPATCH_LEVEL)
   |     3. قراءة السياسة (policy) تحت EX_SPIN_LOCK
   |     4. فحص IoStatus.Information bytes فقط
   |     5. تطبيق كل القواعد (rules) على system buffer
   v
I/O Manager  ->  caller يرى البيانات المُعدّلة
```

- `IoStatus.Information` يحدد المنطقة المُفحوصة.
- `EX_SPIN_LOCK` (via `KeAcquireSpinLockAtDpcLevel`) للأمان على DISPATCH_LEVEL.
- `WdfRequestCompleteWithInformation` يحافظ على عدد البايتات.
- `EvtIoDefault` يعالج كل أنواع الطلبات (read/write/IOCTL/PnP).
- `PowerManaged = WdfFalse` + parallel dispatch.

---

## ملاحظات أمنية

- جهاز التحكم `\\.\TempModFilter` مقيّد لـ SYSTEM و Administrators عبر SDDL
  `D:P(A;;GA;;;SY)(A;;GA;;;BA)`.
- لا patching لـ SSDT/IDT/EPROCESS، لا bypass لـ PatchGuard/DSE/HVCI.
- لا تغييرات دائمة (permanent) على مسار القرص؛ Registry/MAC تتغير وتُستعاد عبر `restore`.
- **لا يغيّر SMBIOS UUID أو serial اللوحة (out of scope for this filter).**

---

## استكشاف الأخطاء (Troubleshooting)

| العرض | السبب | الحل |
|-------|-------|------|
| CreateFile error 2 | Driver غير مُثبّت/مُشغّل | شغّل Install.ps1، أعد التشغيل |
| Error 577 | Test signing / cert | `testsigning on`، Secure Boot off، ثبّت cer المرفق |
| Error 31 / 127 | KMDF مفقود | ثبّت WDK + toolset `WindowsKernelModeDriver10.0` |
| demo error 5 | تحليل PowerShell | شغّل من elevated PowerShell |
| DbgPrint مفقود | debug filter | عيّن `DEFAULT` debug print filter إلى 0xF |
| BSOD | خطأ في Driver | Safe Mode، احذف إدخال UpperFilters |

السجلات: `C:\Windows\INF\setupapi.dev.log`، Event Viewer (System/BugCheck)،
WinDbg `!analyze -v`.

---

## الترخيص (License)

انظر LICENSE. للاستخدام في المختبر/VM فقط. بدون ضمان. استخدم بمسؤولية.
