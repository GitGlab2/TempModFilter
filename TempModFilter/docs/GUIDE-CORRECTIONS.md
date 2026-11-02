# تصحيح كود الدليل الأصلي

القسم 5 من `../Kernel_Level_Drivers_Guide.md` يحتوي نموذجاً تعليمياً مفيداً، لكنه
لا يُترجم ولا يعمل كما هو. هذا الملف يسرد الأخطاء واحداً واحداً، مع المقابل في
`driver/FilterDriver.c`.

---

## 1. `NT_SUCCESS(Params->Type)` — خطأ في النوع

```c
// الدليل
if (NT_SUCCESS(Params->Type)) { ... }
```

`Params->Type` هو `WDF_REQUEST_TYPE` (تعداد: Read/Write/DeviceControl/...)، وليس
`NTSTATUS`. تمريره إلى `NT_SUCCESS` يفحص قيمة تعداد كأنها كود خطأ، فتعطي نتائج
عشوائية.

```c
// المشروع
if (Params->Type == WdfRequestTypeDeviceControl &&
    NT_SUCCESS(Params->IoStatus.Status)) { ... }
```

## 2. `WdfRequestFormatRequestForIoTarget` — دالة غير موجودة

```c
// الدليل
WdfRequestFormatRequestForIoTarget(Request, WdfDeviceGetIoTarget(...), NULL);
```

لا وجود لها في KMDF. الصحيح لفلتر يمرّر الطلب كما وصل (الدالة تُرجع `VOID`):

```c
// المشروع
WdfRequestFormatRequestUsingCurrentType(Request);
```

أو `WdfRequestFormatRequestForIoctl` إن أردت إعادة بناء الطلب بمعاملات مختلفة.

## 3. `WdfRequestComplete(Request, Params->Type)` — حالة إكمال خاطئة

إكمال الطلب بقيمة تعداد يعني أن المستدعي قد يرى `STATUS_SUCCESS` أو خطأً
عشوائياً. الصحيح هو حالة الـ IRP، مع الحفاظ على عدد البايتات:

```c
// المشروع
WdfRequestCompleteWithInformation(
    Request, Params->IoStatus.Status, Params->IoStatus.Information);
```

## 4. `WdfRequestComplete` بعد `WdfRequestSend` تفشل

```c
// الدليل
if (!WdfRequestSend(...)) {
    WdfRequestComplete(Request, WdfRequestGetStatus(Request));
}
```

مقبول شكلياً، لكن `WdfRequestGetStatus` قد يعيد `STATUS_SUCCESS` لأن الطلب لم
يُرسَل أصلاً. الأفضل ضمان حالة فشل صريحة:

```c
// المشروع
status = WdfRequestGetStatus(Request);
WdfRequestComplete(Request, NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status);
```

## 5. الطابور لا يمرّر إلا `DeviceControl` — يتجمّد مكدس التخزين

```c
// الدليل
WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
queueConfig.EvtIoDeviceControl = FilterEvtIoDeviceControl;
```

مع `EvtIoDeviceControl` فقط، أي طلب قراءة/كتابة/IOCTL داخلي لا يجد معالجاً،
فيكمله الإطار بـ `STATUS_INVALID_DEVICE_REQUEST`. الفلتر العلوي في مسار التخزين
يجب أن يمرّر **كل** شيء:

```c
// المشروع
queueConfig.EvtIoDefault = TempModEvtIoDefault;   // كل الأنواع
```

## 6. تعديل ناتج عند `DISPATCH_LEVEL` دون قفل مناسب

روتين الاكتمال قد يعمل عند `DISPATCH_LEVEL`. استخدام `WDFWAITLOCK` هناك غير
مسموح، وقراءة السياسة دون قفل تسبب سباقاً مع `IOCTL` التسليح:

```c
// المشروع
*AtDispatch = (KeGetCurrentIrql() >= DISPATCH_LEVEL);
if (*AtDispatch) KeAcquireSpinLockAtDpcLevel(&State->Lock);
else              KeAcquireSpinLock(&State->Lock, OldIrql);
```

## 7. تعديل الـ Buffer كاملاً بدل الجزء الصالح فقط

`WdfMemoryGetBuffer` يعيد حجم الـ Buffer الكامل، بينما البيانات الفعلية هي
`Params->IoStatus.Information` بايت. مسح بقية الـ Buffer:

- يفسد بيانات قد يقرأها المستدعي،
- وقد يعدّل بايتات لم يكتبها المشغّل الأدنى أصلاً (تسريب من الذاكرة غير المهيّأة).

```c
// المشروع
validLength = bufferLength;
if (Params->IoStatus.Information > 0 &&
    Params->IoStatus.Information < validLength) {
    validLength = (size_t)Params->IoStatus.Information;
}
```

## 8. لا يوجد جهاز تحكّم ولا وسيلة تعطيل

نموذج الدليل يعدّل "شيئاً ما" دون طريقة لإيقافه. المشروع يضيف:

- `\\.\TempModFilter` مع خمسة `IOCTL`s،
- سياسة قابلة للتغيير أثناء التشغيل،
- عدّادات،
- ووضع `pass-through` كامل عند التعطيل.

## 9. أمان جهاز التحكّم

جهاز التحكّم الافتراضي قد يكون متاحاً لمستخدمين لا يجب أن يعدّلوا ذاكرة النواة.
المشروع يفرض SDDL صريحاً:

```c
RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
deviceInit = WdfControlDeviceInitAllocate(Driver, &sddl);
```

(في الدليل لا يوجد جهاز تحكّم أصلاً، لكن هذه نقطة يجب الانتباه لها عند الإضافة.)

## 10. التحقق من صحة المدخلات

أي `IOCTL` يعدّل سلوك النواة يحتاج تحققاً صارماً: إصدار البنية، طول النمط،
الحد الأقصى للاستبدال، وتصفير الحقول المحجوزة. راجع
`TempModValidatePolicy` و`TempModSetPolicy` في `driver/FilterPolicy.c`.

---

## ملاحظة على التسمية

`WdfFdoInitSetFilter` صحيحة (`DeviceInit`، وليس `WdfDeviceInit`). الكود في
الدليل يستخدمها بشكل صحيح. النقطة الوحيدة المرتبطة بها أنها **لا تكفي** وحدها:
يجب أن يكون اسم الخدمة مسجّلاً في `UpperFilters` لمفتاح الفئة، وإلا لن يُستدعى
`EvtDeviceAdd` أبداً — وهذا سبب شائع لظهور "الفلتر لا يعمل" بعد التثبيت.
