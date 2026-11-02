# الوضع المؤقت (جلسة = بوت واحد)

الافتراضي دلوقتي إن الأداة **مش بتسيب أي أثر بعد الريستارت**. الجلسة كلها
بتعيش بوت واحد بس، وأي ريستارت بعدها بيرجّع الجهاز لحالته الطبيعية لوحده.

## إزاي بتشتغل؟

1. تشغّل `Spoofer-Setup.cmd`.
   - بيفعّل Test Signing.
   - بيوقّع الدرايفر ويسجّله كفلتر على كلاس الأقراص (`DiskDrive`).
   - بيسجّل مهمة تشغيل اسمها `TempModFilter-ArmSession`.
2. بتعمل **ريستارت**. البوت ده هو "جلسة السبوفنج":
   - الفلتر بيتربط بالأقراص أثناء البوت.
   - مهمة `ArmSession` بتشتغل بعد ما الفلتر يحمّل وبتعمل حاجتين:
     - بتشيل `TempModFilter` من `UpperFilters` — يعني البوت الجاي مش هيحاول
       يحمّل درايفر متوقّع بـ test signing بعد ما نقفله.
     - بتقفل `testsigning` للبوت الجاي.
     - بتسجّل مهمة `TempModFilter-FinishCleanup` للبوت اللي بعده.
     - **الفلتر المحمّل حاليًا بيفضل شغال** للبوت ده، فالأداة بتشتغل عادي.
3. تستخدم `Spoofer-Run.cmd` في البوت ده.
4. أي **ريستارت** بعد كده بيطلع نظيف:
   - الدرايفر والخدمة والملف بيتشالوا.
   - شهادة الـ test بتتشال من الـ stores.
   - قيم الريجستري (MachineGuid / ProductId / Hostname / …) بترجع من الباك أب.
   - Test Mode بيقف.

## ليه ده آمن؟

- **مستحيل** يحصل إن `testsigning` يتقفل والفلتر لسه متسجل. الترتيب في
  `Session-Cleanup.ps1` بيتأكد إن تسجيل الفلتر اتشال الأول، وبعدها بس بيقفل
  test signing.
- لو الجهاز فصل كهربا أو عمل Blue Screen أثناء الجلسة، مهمة التنظيف هتشتغل
  في البوت الجاي. مش هيفضل عالق في وضع مش قادر يقوم.
- لو مهمة التنظيف فشلت لأي سبب، بتفضل مسجّلة وبتعيد المحاولة في البوت اللي
  بعده.
- لو الفلتر نفسه مش بيتحمّل خالص (مثلًا الدرايفر اترفض)، مهمة التجهيز بتحاول
  3 بوتات وبعدها بتشيل الجلسة كلها لوحدها — يعني مش هتفضل عالق في Test Mode
  للأبد.

## الملفات واللوجات

كل حاجة بتتنسخ في `C:\ProgramData\TempModFilter`:

| الملف | إيه ده |
|---|---|
| `Session-Arm.ps1` | مهمة التجهيز (بوت واحد) |
| `Session-Cleanup.ps1` | التنظيف الكامل |
| `FilterClient.exe` | الأداة نفسها |
| `session.log` | لوج التجهيز |
| `cleanup.log` | لوج التنظيف |
| `identity-backup.txt` | نسخة قيم الريجستري الأصلية (بتتمسح بعد الرجوع) |

لو حصلت مشكلة، `cleanup.log` هو أول حاجة تبص عليها: كل خطوة بتتسجل بنتيجة
`OK` / `WARN` / `ERR`.

## إزاي تتأكد إن كل حاجة رجعت؟

بعد الريستارت النهائي:

```powershell
Get-Service TempModFilter -ErrorAction SilentlyContinue     # المفروض مفيش حاجة
Test-Path C:\Windows\System32\drivers\TempModFilter.sys     # False
(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4D36E967-E325-11CE-BFC1-08002BE10318}').UpperFilters
Get-ScheduledTask -TaskName 'TempModFilter-*' -ErrorAction SilentlyContinue
bcdedit /enum | Select-String testsigning                      # المفروض No
```

## أوامر يدوية

```powershell
# نظّف دلوقتي (شيل الدرايفر + الشهادة + رجّع الريجستري + اقفل Test Mode)
.\scripts\Session-Cleanup.ps1 -RestoreRegistry -RemoveCertificate

# نظّف واعمل ريستارت
.\scripts\Uninstall.ps1 -Reboot
```

ومن منيو `Spoofer-Run.cmd`:

- `10) CLEAN UP NOW` — تنظيف فوري.
- `11) CLEAN UP + RESTART` — تنظيف وريستارت.

## استثناءات لازم تعرفها

- **الهوست نيم (اسم الجهاز):** التغيير بيطبق عند البوت. التنظيف بيرجّع قيمة
  الريجستري، لكن الاسم النشط ممكن يفضل متغيّر لحد ريستارت إضافي.
- **WMI Repository:** لو استخدمت `wmi spoof`، ده مش بيتشال تلقائيًا. استخدم
  `FilterClient.exe wmi spoof --reset-repository` أو ارجع من نسخة الـ WMI.
- **Test Mode Watermark:** أثناء بوت الجلسة نفسه هيفضل مكتوب "Test Mode" في
  الكورنر، لأن الدرايفر محتاج test signing. بعد الريستارت اللي بعده بيختفي.

## الفرق عن الوضع الدائم

- `Spoofer-Setup.cmd` (بدون أي باراميتر) = مؤقت.
- `Install.ps1` من غير `-Temporary` = دائم لحد ما تشغّل `Uninstall.ps1`.
- `Spoofer-Setup.cmd -Persistent` = دائم.

## لو الجهاز كان واقف مش عارف يقوم

هو كان بيحصل لأن الفلتر كان لسه متسجل في `UpperFilters` و`testsigning` اتقفل،
فالويندوز بيرفض يحمّل الدرايفر وبالتالي الستاك بتاع التخزين بيفشل.

الإصلاح:

```cmd
:: من CMD كـ Administrator
bcdedit /set {current} testsigning off
```

ولو لسه فيه درايفر:

```powershell
.\scripts\Session-Cleanup.ps1 -RestoreRegistry -RemoveCertificate
```
