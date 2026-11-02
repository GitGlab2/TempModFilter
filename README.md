# TempModFilter — مشروع التعديل المؤقت على مستوى النواة

[![Build TempModFilter Driver](https://github.com/GitGlab2/TempModFilter/actions/workflows/build.yml/badge.svg)](https://github.com/GitGlab2/TempModFilter/actions/workflows/build.yml)

مشروع عملي كامل مبني على [`docs/Kernel_Level_Drivers_Guide.md`](docs/Kernel_Level_Drivers_Guide.md):
مشغّل فلتر علوي (Upper Filter Driver) بنمط **KMDF** يعترض طلبات الإدخال/الإخراج
(`IRP`) داخل نواة Windows، ويعدّل **نسخة البيانات الموجودة في الذاكرة فقط** قبل
تسليمها لبرنامج المستخدم.

> **هذا مشروع مختبر (Lab) للتعلم والتحقق.** يعمل داخل آلة افتراضية فقط.
> لا تكتب أي بيانات على الأقراص أو الـ Firmware، ولا تحاول تجاوز أي آلية حماية.

---

## 1. الفكرة في سطر واحد

```
التطبيق يطلب بيانات  ->  الفلتر يعترض الرد  ->  يعدّل الـ Buffer في RAM
                     ->  التطبيق يرى النسخة المعدّلة  ->  إعادة التشغيل = كل شيء أصلي
```

التعديل موجود داخل `Non-Paged Pool` / `System Buffer` الخاص بطلب واحد فقط،
وينتهي بإنهاء الطلب أو بإعادة التشغيل. لا يوجد أي أثر على القرص أو الـ NVRAM.

---

## 2. ما يفعله المشروع فعلياً

| المكوّن | الوصف |
| :--- | :--- |
| `TempModFilter.sys` | فلتر علوي لفئة `DiskDrive` يعترض كل الطلبات ويمرّرها للأسفل، ويعدّل حقل الإخراج لطلبات `IRP_MJ_DEVICE_CONTROL` عند اكتمالها |
| `\\.\TempModFilter` | جهاز تحكّم (Control Device) بواجهة `IOCTL` لتسليح/تعطيل السياسة أثناء التشغيل |
| `FilterClient.exe` | عميل إداري: تسليح السياسة، قراءة العدّادات، واختبار Round-Trip يثبت أن التعديل مؤقت |

### سياسة التعديل

السياسة ليست "عنوان ثابت" بل **نمط بايتات** (Pattern) مع بايت بديل:

```
كل تكرار للنمط  "ABC123"  داخل حقل الإخراج  ->  يُستبدل بـ  "******"
```

هذا يجعل المشروع أداة **تنقيح (Redaction)** عامة وواضحة، لا أداة مرتبطة ببنية
جهاز معيّن. يمكن تقييد التعديل على `IOCTL` واحد بعينه عبر `TargetIoControl`.

---

## 3. ما لا يفعله المشروع (حدود متعمّدة)

هذه الحدود مقصودة وموثّقة، وليست نقصاً في التنفيذ:

- ❌ لا يكتب على القرص أو الـ SPI Flash أو الـ EEPROM أو الـ NVRAM.
- ❌ لا يعدّل هياكل النواة (`SSDT`, `IDT`, `EPROCESS`, `KPCR`).
- ❌ لا يتجاوز `PatchGuard` ولا `DSE` ولا `HVCI` ولا `VBS`.
- ❌ لا يخفي نفسه ولا العمليات ولا الملفات.
- ❌ لا يوفّر وصولاً عشوائياً للذاكرة الفيزيائية.
- ❌ لا يغيّر معرّفات العتاد بشكل دائم، ولا يلمس `SMBIOS`/`DMI`.

أي محاولة لتجاوز `PatchGuard` أو `DSE` تخرج عن نطاق هذا المشروع، وتؤدي عملياً
إلى `CRITICAL_STRUCTURE_CORRUPTION` أو رفض تحميل المشغّل (خطأ 577).

---

## 4. بنية الملفات

```
TempModFilter/                     جذر المستودع
├── TempModFilter.sln              حل Visual Studio (x64 فقط)
├── README.md
├── LICENSE
├── .github/workflows/build.yml    بناء تلقائي على GitHub Actions
├── include/
│   └── TempModShared.h            العقد المشترك: IOCTLs + Structs (UM/KM)
├── driver/
│   ├── FilterDriver.c             نقطة الدخول، جهاز التحكّم، مسار البيانات
│   ├── FilterPolicy.c/.h          محرّك التعديل + الحالة المحمية بـ Spinlock
│   ├── FilterDriver.inf           حزمة تثبيت الفلتر كفلتر فئة (DiskDrive)
│   └── FilterDriver.vcxproj
├── user/
│   ├── FilterClient.cpp           عميل الأوامر + اختبار Round-Trip
│   └── FilterClient.vcxproj
├── scripts/
│   ├── Build.ps1                  بناء عبر msbuild + فحص وجود WDK
│   ├── Sign-Test.ps1              شهادة اختبار موقّعة ذاتياً + signtool
│   ├── Install.ps1                نسخ + خدمة + UpperFilters + نسخة احتياطية
│   ├── Uninstall.ps1              إزالة جراحية + استرجاع النسخة الاحتياطية
│   └── Test-RoundTrip.ps1         تحقق شامل قبل/بعد
└── docs/
    ├── Kernel_Level_Drivers_Guide.md   الدليل الأصلي
    └── GUIDE-CORRECTIONS.md            أخطاء الكود في الدليل وتصحيحها
```

---

## 5. المتطلبات

| المتطلب | ملاحظة |
| :--- | :--- |
| Windows 10/11 x64 | داخل **آلة افتراضية** (Hyper-V / VMware) |
| Visual Studio 2022 | حزمة "Desktop development with C++" |
| Windows Driver Kit (WDK) | يوفّر `WindowsKernelModeDriver10.0` و `wdf.h` |
| صلاحيات Administrator | للتثبيت والتوقيع والتشغيل |
| Secure Boot = **Off** في الـ VM | شرط أساسي لـ `testsigning` |

لا يوجد دعم لـ x86 ولا لـ ARM64 في هذه النسخة (البنية نفسها قابلة للنقل، لكن
لم يتم اختبارها).

---

## 6. البناء

```powershell
git clone https://github.com/GitGlab2/TempModFilter.git
cd TempModFilter
.\scripts\Build.ps1                     # Debug|x64
.\scripts\Build.ps1 -Configuration Release
```

المخرجات:

```
build\Debug\x64\driver\TempModFilter.sys
build\Debug\x64\user\FilterClient.exe
```

أو من Visual Studio: افتح `TempModFilter.sln` واختر `Debug|x64`.

**إن فشل البناء:** تأكد أن الـ WDK مثبّت كإضافة داخل Visual Studio، لا كحزمة
منفصلة فقط. رسالة `The builds tools for WindowsKernelModeDriver10.0 cannot be
found` تعني أن الإضافة غير مثبتة.

---

## 7. التوقيع (Test Signing)

```powershell
.\scripts\Sign-Test.ps1
```

يقوم بـ:
1. إنشاء شهادة `CodeSigningCert` ذاتية التوقيع في `LocalMachine\My`.
2. استيرادها إلى `Root` و `TrustedPublisher` (محلياً فقط).
3. توقيع `TempModFilter.sys` بـ `signtool /fd SHA256`.

ثم داخل الـ VM:

```cmd
bcdedit /set testsigning on
```

وأعد التشغيل. للتذكير: `testsigning` يحتاج **إيقاف Secure Boot** في إعدادات
الجهاز الافتراضي (Generation 2).

> الشهادة للاختبار فقط. لا توزّع مشغّلاً موقّعاً بها، ولا تتركها في مخزن
> الجهاز بعد انتهاء العمل.

---

## 8. التثبيت

```powershell
.\scripts\Install.ps1
```

السكربت:

1. يرفض العمل على جهاز غير افتراضي إلا مع `-Force`.
2. يأخذ **نسخة احتياطية** من قيمة `UpperFilters` الحالية إلى
   `C:\ProgramData\TempModFilter\UpperFilters.backup.json`.
3. ينسخ الملف إلى `C:\Windows\System32\drivers\TempModFilter.sys`.
4. ينشئ خدمة kernel (`start= demand`).
5. يضيف `TempModFilter` إلى
   `HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E967-E325-11CE-BFC1-08002BE10318}\UpperFilters`.
6. يطلب إعادة تشغيل.

الفلتر لا يُربط فعلياً إلا عند بدء تشغيل أجهزة الفئة، لذلك **إعادة التشغيل
إلزامية**، وليس `sc start` وحده.

### المسار البديل (INF)

```powershell
.\scripts\Install.ps1 -Mode Inf
```

يستخدم `setupapi!InstallHinfSection DefaultInstall` مع `driver\FilterDriver.inf`.
هذا المسار يحتاج حزمة موقّعة (`.cat`) لتثبيت نظيف على جهاز مقيّد، لذلك المسار
الافتراضي هو `Manual`.

---

## 9. الاستخدام

```powershell
$c = ".\build\Debug\x64\user\FilterClient.exe"

& $c status                     # السياسة الحالية
& $c stats                      # العدّادات
& $c set --pattern "ABC123" --replace 0x2A --ioctl 0x2D1400 --max 8
& $c clear                      # تعطيل فوري (pass-through)
& $c demo --drive 0             # اختبار قبل/بعد على قرص
```

### صيغة النمط

| الصيغة | مثال |
| :--- | :--- |
| نص ASCII | `--pattern "ST1000DM003"` |
| بايتات سداسية | `--pattern "hex:41 42 43"` |

### الاختبار الذاتي

```powershell
.\scripts\Test-RoundTrip.ps1
```

يقرأ الرقم التسلسلي للقرص، يسلّح سياسة تنقيح، يعيد القراءة (يظهر مقنّعاً)،
ثم يلغي السياسة ويعيد القراءة (يعود أصلياً). هذه الخطوة الأخيرة هي الإثبات
العملي أن التعديل **مؤقت**.

مثال للمخرجات:

```
[1/4] serial BEFORE arming the filter : "WD-WCC4N1234567"
[2/4] policy armed for IOCTL 0x002D1400, pattern "WD-WCC4N1234567"
[3/4] serial WITH the filter active    : "***************"
[4/4] serial AFTER clearing the policy : "WD-WCC4N1234567"

    modified in RAM      : yes
    restored after clear : yes
```

---

## 10. الإزالة والتراجع

```powershell
.\scripts\Uninstall.ps1              # إزالة + طلب تأكيد
.\scripts\Uninstall.ps1 -RestoreBackup -Reboot
```

الترتيب الصحيح: إيقاف/حذف الخدمة ← إزالة الاسم من `UpperFilters` (أو استرجاع
النسخة الاحتياطية) ← حذف الملف ← إعادة تشغيل.

> المشغّل **غير قابل للتفريغ أثناء العمل** بشكل متعمّد (لا يوجد `EvtDriverUnload`)،
> تماماً كمعظم فلاتر التخزين. لذلك `sc stop` سيفشل أثناء ارتباطه بالأقراص،
> والانفصال الحقيقي يحدث عند إعادة التشغيل فقط.

**خطة الطوارئ** إذا رفض النظام الإقلاع بعد التثبيت: أقلع في Safe Mode، أو من
WinRE افتح `regedit` واحذف قيمة `UpperFilters` من مفتاح الفئة، ثم أعد التشغيل.
هذه هي النقطة الوحيدة التي قد تحتاج تدخلاً يدوياً، لذلك لا تجرّب على جهاز
إنتاجي.

---

## 11. كيف يعمل داخلياً

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
completion routine (may run at DISPATCH_LEVEL)
   |     3. read policy under EX_SPIN_LOCK
   |     4. scan only IoStatus.Information bytes
   |     5. overwrite pattern matches in the system buffer
   v
I/O Manager  ->  caller sees modified data
```

نقاط التصميم المهمة:

- **`IoStatus.Information` هو الحد الفعلي للبيانات**: لا نلمس بقية الـ Buffer
  لأنه غير مهيّأ وقد يسبب تسريب ذاكرة أو فساداً.
- **`EX_SPIN_LOCK`** وليس `WDFWAITLOCK`: روتين الاكتمال قد يعمل عند
  `DISPATCH_LEVEL`، ويُستخدم `KeAcquireSpinLockAtDpcLevel` في هذه الحالة.
- **`WdfRequestCompleteWithInformation`**: الحفاظ على عدد البايتات المُعادة،
  وإلا يرى المستدعي رداً بطول صفر.
- **`EvtIoDefault`**: الفلتر يجب أن يمرّر كل شيء (قراءة/كتابة/IOCTL داخلي/
  PnP)، وإلا تجمّد مكدس التخزين.
- **`PowerManaged = WdfFalse`** و **`WdfIoQueueDispatchParallel`**: لا نُسلسل
  مكدس التخزين خلف طلب واحد، ولا ندخل في إدارة طاقة لا تخصّنا.

### واجهة التحكّم

| IOCTL | الإدخال | الإخراج |
| :--- | :--- | :--- |
| `IOCTL_TEMPMOD_SET_POLICY` | `TEMPMOD_POLICY` | `TEMPMOD_POLICY` (بعد التطبيع) |
| `IOCTL_TEMPMOD_GET_POLICY` | — | `TEMPMOD_POLICY` |
| `IOCTL_TEMPMOD_GET_STATS` | — | `TEMPMOD_STATS` |
| `IOCTL_TEMPMOD_RESET_STATS` | — | — |
| `IOCTL_TEMPMOD_CLEAR_POLICY` | — | — |

الأرقام والأحجام ثابتة ومُتحقَّق منها وقت الترجمة
(`TEMPMOD_POLICY` = 92 بايت، `TEMPMOD_STATS` = 32 بايت). أي تغيير في البنية
يجب أن يرفع `TEMPMOD_POLICY_VERSION`، والمشغّل يرفض أي إصدار مختلف
بـ `STATUS_REVISION_MISMATCH`.

جهاز التحكّم محمي بـ SDDL:

```
D:P(A;;GA;;;SY)(A;;GA;;;BA)
```

أي: SYSTEM والمجموعة الإدارية فقط، لا `Everyone` ولا `Authenticated Users`.

---

## 12. استكشاف الأخطاء

| العَرَض | السبب المرجّح | الحل |
| :--- | :--- | :--- |
| `CreateFile` يفشل بـ 2 | الخدمة غير محمّلة | أعد التشغيل بعد التثبيت |
| فشل التحميل 577 | التوقيع غير مقبول | `testsigning on` + Secure Boot off + أعد التوقيع |
| فشل التحميل 31 / 127 | تبعية KMDF غير محمّلة | تأكد أن البناء `DriverType=KMDF` وأن `Wdf01000.sys` يعمل |
| `demo` يظهر الرقم الأصلي | الفلتر غير مرتبط بالفئة | تحقق من `UpperFilters` وأعد التشغيل |
| `stats` يعرض `buffers scanned = 0` مع طلبات تمر | الإطار لم يوفّر حقل الإخراج | تأكد أن الطلب `METHOD_BUFFERED` وأن `TargetIoControl` يطابق كود الـ IOCTL |
| `stats` يعرض `replacements = 0` بعد المسح | النمط غير موجود في الناتج | اطبع الناتج الخام وتأكد من البايتات (قد يكون UTF-16) |
| `demo` يفشل بـ 5 | صلاحيات | شغّل PowerShell كمسؤول |
| لا تظهر رسائل `DbgPrint` | مرشّح الطباعة | `reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter" /v DEFAULT /t REG_DWORD /d 0xF /f` ثم أعد التشغيل، أو استخدم WinDbg |
| `BSOD` عند الإقلاع | خطأ في الفلتر | أقلع Safe Mode واحذف `UpperFilters` |

سجلات مفيدة:

```
C:\Windows\INF\setupapi.dev.log
Event Viewer -> System -> BugCheck
!analyze -v  (WinDbg)
```

---

## 13. حالة التحقق والمسؤولية

- الكود مكتوب ليبني بـ WDK/MSBuild، وقد تمت مراجعته وتصحيح ثلاثة أخطاء حرجة
  (نوع القفل `KSPIN_LOCK`، استدعاء `WdfControlFinishInitializing`، وقيمة الإرجاع
  لـ `WdfRequestFormatRequestUsingCurrentType`). لكنه لم يُبنَ ولم يُشغَّل فعلياً
  بعد — يحتاج بيئة Visual Studio + WDK. راجع
  `docs/GUIDE-CORRECTIONS.md` قبل الاعتماد على أي سطر من الدليل الأصلي.
- الفلتر يعمل داخل مسار التخزين؛ أي خطأ فيه = `BSOD` فوري. استخدم VM فقط.
- الاستخدام المشروع: التعلم، وتطوير أدوات تنقيح/خصوصية، واختبار أنظمة المراقبة.
  استخدم هذا المشروع في بيئة تملكها، وبما لا يخالف القوانين وشروط الخدمة.

---

## 14. المراجع

- [Windows Drivers — Microsoft Learn](https://learn.microsoft.com/windows-hardware/drivers/)
- [KMDF Filter Drivers](https://learn.microsoft.com/windows-hardware/drivers/wdf/filter-drivers)
- [WDF I/O Queues](https://learn.microsoft.com/windows-hardware/drivers/wdf/io-queues)
- [WdfRequestFormatRequestUsingCurrentType](https://learn.microsoft.com/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestformatrequestusingcurrenttype)
- [Driver Verifier](https://learn.microsoft.com/windows-hardware/drivers/devtest/driver-verifier)
- [Test Signing](https://learn.microsoft.com/windows-hardware/drivers/install/test-signing)
