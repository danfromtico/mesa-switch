/* SPDX-License-Identifier: MIT */
/* Capture/restore the user's single-display mode from an interactive session.
 * Adapter LUIDs are diagnostic data, not persistent display identities.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

static constexpr DWORD required_fields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;

struct snapshot {
   DWORD magic, version;
   SYSTEMTIME utc;
   WCHAR source_name[CCHDEVICENAME];
   WCHAR monitor_path[128];
   WCHAR adapter_device_id[128];
   DEVMODEW current, registry;
   DISPLAYCONFIG_PATH_INFO path;
   DISPLAYCONFIG_SOURCE_MODE source;
   DISPLAYCONFIG_TARGET_MODE target;
   DWORD system_dpi;
};

static LONG read_state(snapshot &s)
{
   s = {};
   s.magic = 0x53444754; /* TGDS */
   s.version = 1;
   GetSystemTime(&s.utc);
   UINT32 path_count = 0, mode_count = 0;
   std::vector<DISPLAYCONFIG_PATH_INFO> paths;
   std::vector<DISPLAYCONFIG_MODE_INFO> modes;
   LONG result = ERROR_INSUFFICIENT_BUFFER;
   for (unsigned attempt = 0; attempt < 3 && result == ERROR_INSUFFICIENT_BUFFER; attempt++) {
      result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
      if (result != ERROR_SUCCESS)
         return result;
      paths.resize(path_count);
      modes.resize(mode_count);
      result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                                  &mode_count, modes.data(), nullptr);
   }
   if (result != ERROR_SUCCESS)
      return result;
   if (path_count != 1)
      return ERROR_NOT_SUPPORTED;
   s.path = paths[0];
   const DWORD source_index = s.path.sourceInfo.modeInfoIdx;
   const DWORD target_index = s.path.targetInfo.modeInfoIdx;
   if (source_index >= mode_count || target_index >= mode_count ||
       modes[source_index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
       modes[target_index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
      return ERROR_INVALID_DATA;
   s.source = modes[source_index].sourceMode;
   s.target = modes[target_index].targetMode;

   DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
   source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                    s.path.sourceInfo.adapterId, s.path.sourceInfo.id};
   result = DisplayConfigGetDeviceInfo(&source.header);
   if (result != ERROR_SUCCESS)
      return result;
   std::memcpy(s.source_name, source.viewGdiDeviceName, sizeof(s.source_name));
   DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
   target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target),
                    s.path.targetInfo.adapterId, s.path.targetInfo.id};
   result = DisplayConfigGetDeviceInfo(&target.header);
   if (result != ERROR_SUCCESS)
      return result;
   std::memcpy(s.monitor_path, target.monitorDevicePath, sizeof(s.monitor_path));

   DISPLAY_DEVICEW adapter = {};
   adapter.cb = sizeof(adapter);
   for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &adapter, 0); index++) {
      if (!std::wcscmp(adapter.DeviceName, s.source_name)) {
         std::memcpy(s.adapter_device_id, adapter.DeviceID, sizeof(s.adapter_device_id));
         break;
      }
   }
   if (!s.adapter_device_id[0])
      return ERROR_NOT_FOUND;
   s.current.dmSize = sizeof(s.current);
   s.registry.dmSize = sizeof(s.registry);
   if (!EnumDisplaySettingsExW(s.source_name, ENUM_CURRENT_SETTINGS, &s.current, 0) ||
       !EnumDisplaySettingsExW(s.source_name, ENUM_REGISTRY_SETTINGS, &s.registry, 0))
      return ERROR_INVALID_DATA;
   if ((s.current.dmFields & required_fields) != required_fields || s.current.dmDriverExtra)
      return ERROR_NOT_SUPPORTED;
   s.system_dpi = GetDpiForSystem();
   return ERROR_SUCCESS;
}

static void json_string(const WCHAR *value)
{
   char utf8[1024];
   int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, sizeof(utf8), nullptr, nullptr);
   std::putchar('"');
   for (int i = 0; i < length - 1; i++) {
      unsigned char c = utf8[i];
      if (c == '"' || c == '\\')
         std::putchar('\\');
      if (c < 32)
         std::printf("\\u%04x", c);
      else
         std::putchar(c);
   }
   std::putchar('"');
}

static void print_mode(const DEVMODEW &m)
{
   std::printf("{\"width\":%lu,\"height\":%lu,\"orientation\":%lu,\"bpp\":%lu,"
               "\"hz\":%lu,\"x\":%ld,\"y\":%ld,\"flags\":%lu,\"fields\":%lu}",
               m.dmPelsWidth, m.dmPelsHeight, m.dmDisplayOrientation, m.dmBitsPerPel,
               m.dmDisplayFrequency, m.dmPosition.x, m.dmPosition.y, m.dmDisplayFlags, m.dmFields);
}

struct desktop_context {
   DWORD session = MAXDWORD, console_session = MAXDWORD, input_error = 0;
   WCHAR desktop[128] = {}, input[128] = {};
};

static desktop_context read_desktop()
{
   desktop_context context;
   DWORD bytes = 0;
   ProcessIdToSessionId(GetCurrentProcessId(), &context.session);
   context.console_session = WTSGetActiveConsoleSessionId();
   GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                             context.desktop, sizeof(context.desktop), &bytes);
   HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
   context.input_error = input ? ERROR_SUCCESS : GetLastError();
   if (input) {
      if (!GetUserObjectInformationW(input, UOI_NAME, context.input, sizeof(context.input), &bytes))
         context.input_error = GetLastError();
      CloseDesktop(input);
   }
   return context;
}

static bool visible_desktop()
{
   const auto context = read_desktop();
   return context.session == context.console_session && !context.input_error &&
          context.desktop[0] && !std::wcscmp(context.desktop, context.input);
}

static LONG wake_screensaver(const snapshot &state)
{
   if (visible_desktop())
      return ERROR_SUCCESS;
   const auto desktop = read_desktop();
   if (desktop.session != desktop.console_session || desktop.input_error ||
       std::wcscmp(desktop.input, L"Screen-saver"))
      return ERROR_ACCESS_DENIED;
   if (!InitializeTouchInjection(1, TOUCH_FEEDBACK_NONE)) {
      DWORD error = GetLastError();
      std::fprintf(stderr, "InitializeTouchInjection: %lu\n", error);
      return error;
   }
   POINTER_TOUCH_INFO touch = {};
   touch.pointerInfo.pointerType = PT_TOUCH;
   touch.pointerInfo.pointerId = 1;
   touch.pointerInfo.ptPixelLocation = {
      state.current.dmPosition.x + (LONG)state.current.dmPelsWidth / 2,
      state.current.dmPosition.y + (LONG)state.current.dmPelsHeight / 2};
   touch.pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
   if (!InjectTouchInput(1, &touch)) {
      DWORD error = GetLastError();
      std::fprintf(stderr, "InjectTouchInput down: %lu\n", error);
      return error;
   }
   Sleep(20);
   touch.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
   DWORD error = InjectTouchInput(1, &touch) ? ERROR_SUCCESS : GetLastError();
   Sleep(20);
   touch.pointerInfo.pointerFlags = POINTER_FLAG_UP | (error ? POINTER_FLAG_CANCELED : 0);
   if (!InjectTouchInput(1, &touch) && !error)
      error = GetLastError();
   if (error) {
      std::fprintf(stderr, "InjectTouchInput release: %lu\n", error);
      return error;
   }
   for (unsigned i = 0; i < 10; ++i) {
      if (visible_desktop()) return ERROR_SUCCESS;
      Sleep(50);
   }
   return ERROR_TIMEOUT;
}

static void print_state(const snapshot &s, const WCHAR *operation, bool changed, LONG status,
                        UINT32 adjustment = 0, LONG strict_status = ERROR_SUCCESS)
{
   const auto context = read_desktop();
   std::printf("{\"operation\":"); json_string(operation);
   std::printf(",\"session\":%lu,\"console_session\":%lu,\"desktop\":",
               context.session, context.console_session);
   json_string(context.desktop);
   std::printf(",\"input_desktop\":"); json_string(context.input);
   std::printf(",\"input_desktop_error\":%lu", context.input_error);
   std::printf(",\"changed\":%s,\"status\":%ld,\"sdc_adjustment\":%u,"
               "\"strict_validation_status\":%ld,\"source_name\":",
               changed ? "true" : "false", status, adjustment, strict_status);
   json_string(s.source_name);
   std::printf(",\"monitor_path\":"); json_string(s.monitor_path);
   std::printf(",\"adapter_device_id\":"); json_string(s.adapter_device_id);
   std::printf(",\"utc\":\"%04u-%02u-%02uT%02u:%02u:%02uZ\",\"system_dpi\":%lu,\"current\":",
               s.utc.wYear, s.utc.wMonth, s.utc.wDay, s.utc.wHour, s.utc.wMinute, s.utc.wSecond, s.system_dpi);
   print_mode(s.current);
   std::printf(",\"registry\":"); print_mode(s.registry);
   std::printf(",\"source_mode\":{\"width\":%u,\"height\":%u},"
               "\"target_signal\":{\"width\":%u,\"height\":%u},\"path_rotation\":%u}\n",
               s.source.width, s.source.height, s.target.targetVideoSignalInfo.activeSize.cx,
               s.target.targetVideoSignalInfo.activeSize.cy, s.path.targetInfo.rotation);
}

static bool same_mode(const DEVMODEW &a, const DEVMODEW &b)
{
   return a.dmPelsWidth == b.dmPelsWidth && a.dmPelsHeight == b.dmPelsHeight &&
      a.dmDisplayOrientation == b.dmDisplayOrientation && a.dmBitsPerPel == b.dmBitsPerPel &&
      a.dmDisplayFrequency == b.dmDisplayFrequency && a.dmDisplayFlags == b.dmDisplayFlags &&
      a.dmPosition.x == b.dmPosition.x && a.dmPosition.y == b.dmPosition.y;
}

static LONG configure(const snapshot &current, const snapshot &saved, UINT32 flags)
{
   /* Rebind the saved modes to the current adapter, whose LUID can change
    * across the driver rollback/reboot. Use CCD timing data rather than the
    * legacy DEVMODE refresh value reported by BasicDisplay. */
   DISPLAYCONFIG_PATH_INFO path = current.path;
   DISPLAYCONFIG_MODE_INFO modes[2] = {};
   modes[0].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
   modes[0].id = path.sourceInfo.id;
   modes[0].adapterId = path.sourceInfo.adapterId;
   modes[0].sourceMode = saved.source;
   modes[1].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
   modes[1].id = path.targetInfo.id;
   modes[1].adapterId = path.targetInfo.adapterId;
   modes[1].targetMode = saved.target;
   path.sourceInfo.modeInfoIdx = 0;
   path.targetInfo.modeInfoIdx = 1;
   path.targetInfo.rotation = saved.path.targetInfo.rotation;
   path.targetInfo.scaling = saved.path.targetInfo.scaling;
   path.targetInfo.refreshRate = saved.path.targetInfo.refreshRate;
   path.targetInfo.scanLineOrdering = saved.path.targetInfo.scanLineOrdering;
   return SetDisplayConfig(1, &path, 2, modes, flags | SDC_USE_SUPPLIED_DISPLAY_CONFIG);
}

static LONG validate_configuration(const snapshot &current, const snapshot &desired,
                                   UINT32 &adjustment, LONG &strict_status)
{
   adjustment = 0;
   strict_status = configure(current, desired, SDC_VALIDATE);
   if (strict_status != ERROR_BAD_CONFIGURATION)
      return strict_status;
   /* CCD can reject even its current source/target pair when best-mode
    * selection needs to reconcile details not represented by CCD. The API
    * documents ALLOW_CHANGES for this case. Applying still requires exact
    * geometry, orientation and panel timing checks below. */
   adjustment = SDC_ALLOW_CHANGES;
   return configure(current, desired, SDC_VALIDATE | adjustment);
}

static bool same_signal(const DISPLAYCONFIG_VIDEO_SIGNAL_INFO &a,
                        const DISPLAYCONFIG_VIDEO_SIGNAL_INFO &b)
{
   return a.pixelRate == b.pixelRate && a.activeSize.cx == b.activeSize.cx &&
      a.activeSize.cy == b.activeSize.cy && a.totalSize.cx == b.totalSize.cx &&
      a.totalSize.cy == b.totalSize.cy && a.scanLineOrdering == b.scanLineOrdering &&
      a.hSyncFreq.Denominator && b.hSyncFreq.Denominator &&
      a.vSyncFreq.Denominator && b.vSyncFreq.Denominator &&
      (UINT64)a.hSyncFreq.Numerator * b.hSyncFreq.Denominator ==
         (UINT64)b.hSyncFreq.Numerator * a.hSyncFreq.Denominator &&
      (UINT64)a.vSyncFreq.Numerator * b.vSyncFreq.Denominator ==
         (UINT64)b.vSyncFreq.Numerator * a.vSyncFreq.Denominator;
}

static int orient(const snapshot &current, const WCHAR *operation, const WCHAR *angle)
{
   unsigned quarter;
   if (!std::wcscmp(angle, L"0")) quarter = 0;
   else if (!std::wcscmp(angle, L"90")) quarter = 1;
   else if (!std::wcscmp(angle, L"180")) quarter = 2;
   else if (!std::wcscmp(angle, L"270")) quarter = 3;
   else { std::fprintf(stderr, "Expected a CCD clockwise angle: 0, 90, 180 or 270\n"); return 2; }
   if (!visible_desktop()) {
      print_state(current, operation, false, ERROR_ACCESS_DENIED);
      return 1;
   }
   snapshot desired = current;
   const auto &native = current.target.targetVideoSignalInfo.activeSize;
   /* This fixed-panel KMD enumerates the unrotated source. Windows exposes
    * the oriented desktop through DEVMODE, while CCD keeps that source's
    * native extent. Check those two coordinate spaces independently. */
   desired.source.width = native.cx;
   desired.source.height = native.cy;
   const DWORD desktop_width = quarter & 1 ? native.cy : native.cx;
   const DWORD desktop_height = quarter & 1 ? native.cx : native.cy;
   desired.path.targetInfo.rotation = static_cast<DISPLAYCONFIG_ROTATION>(quarter + 1);
   desired.path.targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
   const bool test = !std::wcscmp(operation, L"rotation-test");
   const bool save = !std::wcscmp(operation, L"rotation-save");
   /* CCD rotation is clockwise. Keep the physical target timing and adapter
    * identity, and verify the selected geometry after any mode adjustment. */
   UINT32 adjustment;
   LONG strict_status;
   LONG result = validate_configuration(current, desired, adjustment, strict_status);
   bool changed = false;
   if (!result && !test) {
      result = configure(current, desired, SDC_APPLY | adjustment | (save ? SDC_SAVE_TO_DATABASE : 0));
      changed = !result && (current.source.width != desired.source.width ||
         current.source.height != desired.source.height ||
         current.path.targetInfo.rotation != desired.path.targetInfo.rotation);
   }
   snapshot after;
   LONG read_result = read_state(after);
   if (!result) result = read_result;
   const snapshot &expected = test ? current : desired;
   if (!result && (after.source.width != expected.source.width ||
       after.source.height != expected.source.height ||
       after.current.dmPelsWidth != (test ? current.current.dmPelsWidth : desktop_width) ||
       after.current.dmPelsHeight != (test ? current.current.dmPelsHeight : desktop_height) ||
       after.current.dmBitsPerPel != current.current.dmBitsPerPel ||
       after.current.dmDisplayFrequency != current.current.dmDisplayFrequency ||
       after.source.pixelFormat != expected.source.pixelFormat ||
       after.source.position.x != expected.source.position.x ||
       after.source.position.y != expected.source.position.y ||
       after.path.targetInfo.rotation != expected.path.targetInfo.rotation ||
       !same_signal(after.target.targetVideoSignalInfo, current.target.targetVideoSignalInfo)))
      result = ERROR_INVALID_DATA;
   print_state(read_result ? current : after, operation, changed, result, adjustment, strict_status);
   return result ? 1 : 0;
}

int wmain(int argc, WCHAR **argv)
{
   const bool rotation = argc >= 2 && (!std::wcscmp(argv[1], L"rotation-test") ||
      !std::wcscmp(argv[1], L"rotation") || !std::wcscmp(argv[1], L"rotation-save"));
   if (argc < 2 || argc > 3 || (std::wcscmp(argv[1], L"show") && std::wcscmp(argv[1], L"visible") &&
       std::wcscmp(argv[1], L"wake") &&
       std::wcscmp(argv[1], L"save") && std::wcscmp(argv[1], L"verify") &&
       std::wcscmp(argv[1], L"test") && std::wcscmp(argv[1], L"restore") && !rotation) ||
       ((!std::wcscmp(argv[1], L"show") || !std::wcscmp(argv[1], L"visible") || !std::wcscmp(argv[1], L"wake")) ? argc != 2 : argc != 3)) {
      std::fprintf(stderr, "usage: display-state.exe {show|visible|wake} | {save|verify|test|restore} snapshot.bin | {rotation-test|rotation|rotation-save} {0|90|180|270}\n");
      return 2;
   }
   SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
   snapshot current;
   LONG result = read_state(current);
   if (result) {
      std::fprintf(stderr, "Read display state failed: %ld (use an interactive single-display session)\n", result);
      return 1;
   }
   if (rotation)
      return orient(current, argv[1], argv[2]);
   if (!std::wcscmp(argv[1], L"show")) {
      print_state(current, argv[1], false, 0);
      return 0;
   }
   if (!std::wcscmp(argv[1], L"visible")) {
      bool visible = visible_desktop();
      print_state(current, argv[1], false, visible ? ERROR_SUCCESS : ERROR_ACCESS_DENIED);
      return visible ? 0 : 1;
   }
   if (!std::wcscmp(argv[1], L"wake")) {
      result = wake_screensaver(current);
      snapshot after;
      LONG read_result = read_state(after);
      print_state(read_result ? current : after, argv[1], false, result ? result : read_result);
      return result || read_result ? 1 : 0;
   }
   if (!std::wcscmp(argv[1], L"save")) {
      HANDLE file = CreateFileW(argv[2], GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      DWORD bytes = 0;
      bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, &current, sizeof(current), &bytes, nullptr) &&
                bytes == sizeof(current) && FlushFileBuffers(file);
      if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
      if (!ok) { std::fprintf(stderr, "Snapshot write failed: %lu\n", GetLastError()); return 1; }
      print_state(current, argv[1], false, 0);
      return 0;
   }
   snapshot saved = {};
   FILE *file = _wfopen(argv[2], L"rb");
   bool ok = file && std::fread(&saved, 1, sizeof(saved), file) == sizeof(saved) && std::fgetc(file) == EOF;
   if (file) std::fclose(file);
   if (!ok || saved.magic != current.magic || saved.version != current.version ||
       saved.current.dmSize != sizeof(DEVMODEW) || saved.current.dmDriverExtra ||
       (saved.current.dmFields & required_fields) != required_fields ||
       saved.current.dmDisplayOrientation > DMDO_270 || !saved.current.dmPelsWidth || !saved.current.dmPelsHeight ||
       saved.monitor_path[127] || saved.adapter_device_id[127] ||
       (saved.monitor_path[0] ? std::wcscmp(saved.monitor_path, current.monitor_path) :
                               std::wcscmp(saved.adapter_device_id, current.adapter_device_id))) {
      std::fprintf(stderr, "Snapshot is invalid or belongs to a different display\n");
      return 1;
   }
   bool matches = same_mode(saved.current, current.current) &&
                   saved.path.targetInfo.rotation == current.path.targetInfo.rotation;
   if (!std::wcscmp(argv[1], L"verify")) {
      print_state(current, argv[1], false, matches ? 0 : ERROR_INVALID_DATA);
      return matches ? 0 : 1;
   }
   if (!std::wcscmp(argv[1], L"restore") && matches) {
      print_state(current, argv[1], false, 0);
      return 0;
   }
   if (!visible_desktop()) {
      print_state(current, argv[1], false, ERROR_ACCESS_DENIED);
      return 1;
   }
   UINT32 adjustment;
   LONG strict_status;
   result = validate_configuration(current, saved, adjustment, strict_status);
   if (result != ERROR_SUCCESS) {
      print_state(current, argv[1], false, result, adjustment, strict_status);
      return 1;
   }
   bool changed = false;
   if (!std::wcscmp(argv[1], L"restore") && !matches) {
      result = configure(current, saved, SDC_APPLY | SDC_SAVE_TO_DATABASE | adjustment);
      if (result != ERROR_SUCCESS) {
         print_state(current, argv[1], false, result, adjustment, strict_status);
         return 1;
      }
      changed = true;
   }
   const snapshot &expected = !std::wcscmp(argv[1], L"test") ? current : saved;
   snapshot after;
   result = read_state(after);
   if (!result && (!same_mode(expected.current, after.current) ||
       expected.path.targetInfo.rotation != after.path.targetInfo.rotation))
      result = ERROR_INVALID_DATA;
   print_state(after, argv[1], changed, result, adjustment, strict_status);
   return result ? 1 : 0;
}
