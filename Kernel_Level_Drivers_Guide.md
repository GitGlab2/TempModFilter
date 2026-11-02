# الدليل الشامل لهندسة التعديل المؤقت على مستوى النواة
## (Kernel-Level In-Memory Modification & Filter Drivers)

---

## الفهرس
1. [المقدمة والمفاهيم الأساسية](#1-المقدمة-والمفاهيم-الأساسية)
2. [المقارنة الشاملة: التعديل المؤقت مقابل التعديل الدائم](#2-المقارنة-الشاملة-التعديل-المؤقت-مقابل-التعديل-الدائم)
3. [البنية المعمارية لتدفق البيانات في نظام Windows](#3-البنية-المعمارية-لتدفق-البيانات-في-نظام-windows)
4. [التطوير الرسمي عبر WDK و KMDF (برامج تشغيل الفلاتر)](#4-التطوير-الرسمي-عبر-wdk-و-kmdf-برامج-تشغيل-الفلاتر)
5. [الكود البرمجي وشرح آلية الاعتراض والتعديل](#5-الكود-البرمجي-وشرح-آلية-الاعتراض-والتعديل)
6. [بيئة الاختبار والتصحيح الآمنة (Testing & Debugging)](#6-بيئة-الاختبار-والتصحيح-الآمنة-testing--debugging)
7. [آليات الحماية في أنظمة التشغيل الحديثة (Kernel Security)](#7-آليات-الحماية-في-أنظمة-التشغيل-الحديثة-kernel-security)
8. [خصوصية عتاد المؤسسات (HP ZBook & Sure Start)](#8-خصوصية-عتاد-المؤسسات-hp-zbook--sure-start)
9. [البدائل الآمنة لإدارة معرّفات النظام وبيئات العمل](#9-البدائل-الآمنة-لإدارة-معرفات-النظام-وبيئات-العمل)
10. [المراجع والمصادر الرسمية](#10-المراجع-والمصادر-الرسمية)

---

## 1. المقدمة والمفاهيم الأساسية

### 1.1 ما هو التعديل على مستوى النواة (Kernel Space / Ring 0)؟
ينقسم نموذج حماية المعالج في أنظمة التشغيل الحديثة (مثل Windows و Linux) إلى مستويات صلاحيات برمجية تُعرف بحلقات الحماية (**Protection Rings**):
* **مستوى المستخدم (User Mode / Ring 3):** تعمل فيه جميع البرامج والتطبيقات العادية والمتصفحات. لا تمتلك هذه الطبقة حق الوصول المباشر إلى الذاكرة الفيزيائية أو منافذ العتاد.
* **مستوى النواة (Kernel Mode / Ring 0):** تعمل فيه نواة النظام (`ntoskrnl.exe`) وبرامج تشغيل الأجهزة (**Device Drivers**). يمتلك الكود المنفذ هنا صلاحيات مطلقة لقراءة وتعديل أي مكان في الذاكرة ومخاطبة العتاد مباشرة.

### 1.2 لماذا يُسمى التعديل "مؤقتاً" (In-Memory / Volatile)؟
* **الذاكرة المتطايرة (Volatile Memory):** يحدث التعديل حصراً داخل ذاكرة الوصول العشوائي (**RAM**) وفي المساحات الخاصة بالنواة (مثل **Non-Paged Pool**).
* **غياب التخزين الدائم:** لا يقوم مشغّل النواة بكتابة أو حرق أي بيانات على وسائط التخزين غير المتطايرة (**Non-Volatile Storage**) كأقراص التخزين أو شرائح الـ **ROM / EEPROM / SPI Flash**.
* **دورة الحياة:** بمجرد انقطاع التيار الكهربائي أو إعادة تشغيل الجهاز (**Reboot**)، تُمسح الذاكرة العشوائية بالكامل، ويقوم النظام بإعادة بناء هياكل النواة وجداول الاستدعاء من حالتها النظيفة، مما يعيد كل شيء إلى طبيعته بنسبة 100%.

---

## 2. المقارنة الشاملة: التعديل المؤقت مقابل التعديل الدائم

| معيار المقارنة | التعديل المؤقت (Kernel Drivers) | التعديل الدائم (Firmware / EEPROM) |
| :--- | :--- | :--- |
| **المكان الفيزيائي** | ذاكرة الوصول العشوائي (**RAM**). | شرائح اللوحة الأم (**SPI Flash/EEPROM/NVRAM**). |
| **طريقة العمل** | اعتراض استعلامات النظام في الذاكرة لحظياً. | إعادة كتابة كود أو بيانات الـ Firmware بشكل صلب. |
| **الاستمرارية** | ينتهي فوراً بإعادة تشغيل الجهاز. | مستمر وثابت حتى مع فورمات أو تغيير النظام. |
| **الخطر على العتاد** | منعدم عتادياً؛ أقصى خطر انهيار برمجي مؤقت (**BSOD**). | خطر عالي جداً لإتلاف اللوحة الأم نهائياً (**Bricking**). |
| **الأمان الرقمي** | يمكن ضبطه وتطويره رسمياً كـ Filter Driver. | يتطلب كسر حماية الـ BIOS وتجاوز توقيع المصنع. |
| **سهولة التراجع** | مجرد إعادة تشغيل أو إلغاء تثبيت المشغّل. | يتطلب إعادة برمجة الشريحة يدوياً (**EEPROM Programmer**). |

---

## 3. البنية المعمارية لتدفق البيانات في نظام Windows

عندما تطلب برمجية في مستوى المستخدم الاستعلام عن خصائص عتاد معين (مثل الأقراص، بطاقة الشبكة، أو اللوحة الأم)، تمر العملية عبر مكدس الأجهزة (**Device Stack**):

```
+-------------------------------------------------------+
|                 برامج طبقة المستخدم (User Mode)           |
+-------------------------------------------------------+
                           │ (استدعاء مثل: DeviceIoControl)
                           ▼
+-------------------------------------------------------+
|          مدير الإدخال والإخراج (I/O Manager)          |
|         (يقوم بتوليد حزمة الطلب: IRP Packet)          |
+-------------------------------------------------------+
                           │
                           ▼
┌───────────────────────────────────────────────────────┐
│               مكدس الأجهزة (Device Stack)             │
│                                                       │
│  [1] Upper Filter Driver (مشغّل الفلتر العلوي)        │ <--- الاعتراض والتعديل
│             │                                         │
│             ▼                                         │
│  [2] Functional Device Object - FDO (المشغّل الأساسي) │
│             │                                         │
│             ▼                                         │
│  [3] Lower Filter Driver (مشغّل الفلتر السفلي)        │
│             │                                         │
│             ▼                                         │
│  [4] Physical Device Object - PDO (مشغّل المنفذ/Bus)  │
└───────────────────────────────────────────────────────┘
                           │
                           ▼
+-------------------------------------------------------+
|             العتاد الفيزيائي (Physical Hardware)       |
+-------------------------------------------------------+
```

### عناصر المكدس:
1. **حزمة طلب الإدخال/الإخراج (IRP - I/O Request Packet):** هيكل بيانات يتم إنشاؤه في النواة لنقل أوامر القراءة والكتابة والتحكم عبر طبقات المكدس.
2. **مشغّل الفلتر العلوي (Upper Filter):** طبقة توضع فوق المشغل الأساسي؛ وظيفتها مراقبة أو تعديل البيانات الذاهبة أو العائدة من المشغّل الأساسي.
3. **دالة الاكتمال (Completion Routine):** آلية يحددها الفلتر تُخبر النظام: "عندما ينتهي المشغل الأساسي من قراءة العتاد وتجهيز البيانات، مرر البيانات لي أولاً قبل تسليمها لمستوى المستخدم".

---

## 4. التطوير الرسمي عبر WDK و KMDF (برامج تشغيل الفلاتر)

لتطوير مشغل فلتر نظامي ومعتمد هندسياً، تعتمد مايكروسوفت إطار العمل **KMDF (Kernel-Mode Driver Framework)**.

### متطلبات البيئة:
1. **Visual Studio:** يتضمن حزمة تطوير C++.
2. **Windows Driver Kit (WDK):** يوفر أدوات ومكتبات بناء برامج التشغيل.
3. **بيئة تشغيل معزولة:** آلة افتراضية (**Hyper-V VM**) لتجربة المشغل دون تعريض الجهاز المضيف للخطر.

---

## 5. الكود البرمجي وشرح آلية الاعتراض والتعديل

فيما يلي هيكل كامل لمشغل فلتر مبني بنظام KMDF يوضح كيفية تسجيل المشغل كفلتر واعتراض البيانات في الذاكرة الحية:

### أ. ملف المشغل الأساسي (`FilterDriver.c`)

```c
#include <ntddk.h>
#include <wdf.h>

// الإعلانات المسبقة للدوال
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL FilterEvtIoDeviceControl;
EVT_WDF_REQUEST_COMPLETION_ROUTINE FilterRequestCompletionRoutine;

// 1. نقطة دخول المشغل الرئيسية
NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);

    // إنشاء كائن المشغل في بيئة KMDF
    return WdfDriverCreate(
        DriverObject, 
        RegistryPath, 
        WDF_NO_OBJECT_ATTRIBUTES, 
        &config, 
        WDF_NO_HANDLE
    );
}

// 2. دالة ربط المشغل بمكدس الجهاز
NTSTATUS EvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    UNREFERENCED_PARAMETER(Driver);

    // [أساسي]: إبلاغ النظام بأن هذا المشغل يعمل كـ Filter
    WdfFdoInitSetFilter(DeviceInit);

    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // إعداد طابور لمعالجة الطلبات
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = FilterEvtIoDeviceControl;

    WDFQUEUE queue;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    return status;
}

// 3. اعتراض أوامر التحكم بالجهاز (IOCTL)
VOID FilterEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    // تجهيز الطلب لإرساله للطبقة الأدنى في المكدس
    WdfRequestFormatRequestForIoTarget(
        Request,
        WdfDeviceGetIoTarget(WdfIoQueueGetDevice(Queue)),
        NULL
    );

    // تسجيل دالة الاكتمال لمراقبة النتيجة العائدة من العتاد
    WdfRequestSetCompletionRoutine(
        Request,
        FilterRequestCompletionRoutine,
        (WDFCONTEXT)(ULONG_PTR)IoControlCode
    );

    // إرسال الطلب لأسفل المكدس
    if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(WdfIoQueueGetDevice(Queue)), WDF_NO_SEND_OPTIONS)) {
        WdfRequestComplete(Request, WdfRequestGetStatus(Request));
    }
}

// 4. دالة الاكتمال: هنا يحدث التعديل المؤقت في الذاكرة (RAM)
VOID FilterRequestCompletionRoutine(
    _In_ WDFREQUEST                     Request,
    _In_ WDFIOTARGET                    Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT                     Context
)
{
    UNREFERENCED_PARAMETER(Target);
    ULONG ioctl = (ULONG)(ULONG_PTR)Context;

    // التحقق من نجاح عملية القراءة من العتاد الحقيقي
    if (NT_SUCCESS(Params->Type)) {
        WDFMEMORY memory = Params->Parameters.Ioctl.Output.Buffer;
        if (memory != NULL) {
            size_t bufferSize = 0;
            PVOID buffer = WdfMemoryGetBuffer(memory, &bufferSize);

            if (buffer != NULL && bufferSize > 0) {
                /*
                 * [التعديل المؤقت]:
                 * البيانات هنا موجودة في الـ Buffer في الذاكرة العشوائية.
                 * يمكن قراءتها، تنقيحها، أو تعديل قيم معينة لحظياً.
                 * هذا التعديل لا يمس العتاد الأصلي وسيزول فور تفريغ الذاكرة.
                 */
            }
        }
    }

    // إكمال الطلب وتسليم البيانات المعدلة للبرنامج
    WdfRequestComplete(Request, Params->Type);
}
```

---

## 6. بيئة الاختبار والتصحيح الآمنة (Testing & Debugging)

لتجنب حدوث أي انهيار لنظام التشغيل الأساسي:

1. **استخدام آلة افتراضية (Hyper-V VM):**
   * إنشاء نظام Windows 10 معزول داخل Hyper-V.
2. **تفعيل وضع الاختبار (Test Mode):**
   * نظراً لأن المشغلات أثناء التطوير لا تحمل توقيعاً معتمداً من مايكروسوفت، يجب تفعيل وضع الاختبار داخل الآلة الافتراضية:
     ```cmd
     bcdedit /set testsigning on
     ```
3. **التصحيح عبر الشبكة (Kernel Debugging with WinDbg):**
   * ربط بيئة WinDbg على الجهاز الأساسي بالآلة الافتراضية عبر بروتوكول KDNET لمراقبة تنفيذ الكود ووضع نقاط التوقف (**Breakpoints**).
4. **أداة Driver Verifier:**
   * تشغيل أداة `verifier.exe` لاختبار سلامة المشغل من تسريبات الذاكرة وتعارض مستويات الـ IRQL.

---

## 7. آليات الحماية في أنظمة التشغيل الحديثة (Kernel Security)

تفرض أنظمة 64-bit الحديثة آليات دفاعية مشددة لمنع أي تعديل غير منضبط في النواة:

* **Kernel Patch Protection (PatchGuard):**
  * آلية مدمجة في ويندوز تقوم بفحص دوري لهياكل النواة الحساسة (مثل جداول مقاطعات المعالج IDT وجداول نداءات النظام SSDT). إذا رصدت تعديلاً يدوياً مباشراً، تقوم بإيقاف النظام فوراً بشاشة زرقاء (**CRITICAL_STRUCTURE_CORRUPTION**).
* **فرض التوقيع الرقمي (Driver Signature Enforcement - DSE):**
  * يمنع تحميل أي مشغل نواة ما لم يكن موقّعاً رقمياً بشهادة معتمدة من بوابة مطوري مايكروسوفت (**WHQL Portal**).
* **الأمان القائم على المحاكاة الافتراضية (VBS / HVCI):**
  * عزل مساحة النواة داخل بيئة افتراضية تابعة للـ Hypervisor تجعل صفحات الذاكرة البرمجية غير قابلة للتعديل (**W^X - Write XOR Execute**)، مما يمنع حقن الأكواد غير الموثوقة.

---

## 8. خصوصية عتاد المؤسسات (HP ZBook & Sure Start)

أجهزة **HP ZBook** مزودة بطبقات أمان مادية متقدمة تجعل التعديلات غير النظامية شديدة الخطورة:

1. **معالج الأمان HP Sure Start:**
   * وجود شريحة أمان مادية مستقلة تفحص سلامة الـ BIOS والـ DMI في كل عملية تشغيل.
   * أي محاولة لتعديل معرّفات اللوحة الأم أو الـ Serial Number عبر برامج الفلاش أو أدوات الـ DMI غير الموثقة تعتبر "هجوماً عتادياً"، ويقوم النظام إما باسترجاع النسخة السليمة أو قفل اللوحة الأم نهائياً (**Bricking**).
2. **شريحة TPM 2.0 والتشفير:**
   * ترتبط معرّفات الجهاز بمفاتيح التشفير المادية (مثل BitLocker). أي تلاعب عشوائي بالعتاد قد يؤدي لفقدان مفاتيح التشفير وصعوبة استعادة البيانات.

---

## 9. البدائل الآمنة لإدارة معرّفات النظام وبيئات العمل

إذا كان الهدف هو إعداد بيئة نظيفة للتطوير والاختبار وتجنب المعرّفات القديمة:

1. **البيئات الافتراضية (الخيار الأفضل والآمن بنسبة 100%):**
   * استخدام **Hyper-V** أو **VMware**.
   * تمنحك الآلة الافتراضية عتاداً وهمياً بالكامل (لوحة أم افتراضية، أرقام أقراص وهمية، وعناوين شبكة جديدة) دون أي خطر على عتاد الجهاز الفيزيائي.
2. **إعادة تعيين معرّفات الويندوز عبر Sysprep:**
   * أداة التعميم الرسمية من مايكروسوفت لإعادة توليد معرّف الأمان الفريد (**SID**):
     ```cmd
     %windir%\system32\sysprep\sysprep.exe /generalize /oobe /reboot
     ```
3. **تغيير عنوان MAC من إعدادات النظام:**
   * الذهاب إلى `Device Manager` -> `Network Adapters` -> خصائص الكرت -> `Advanced`.
   * تعديل قيمة **Locally Administered Address** رسمياً دون الحاجة لأي مشغلات خارجية.

---

## 10. المراجع والمصادر الرسمية

* [وثائق مايكروسوفت الرسمية لتطوير برامج التشغيل (Microsoft Learn: Windows Drivers)](https://learn.microsoft.com/en-us/windows-hardware/drivers/)
* [دليل معمارية KMDF لمشغلات الفلاتر (Filter Drivers Architecture)](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/filter-drivers)
* [توثيق فحص المشغلات وتصحيحها (Driver Verifier & WinDbg)](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/driver-verifier)

