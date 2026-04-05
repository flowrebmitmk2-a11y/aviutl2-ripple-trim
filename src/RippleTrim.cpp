#define NOMINMAX
#include <windows.h>

#include <climits>
#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include <aviutl2_sdk/plugin2.h>
#include <aviutl2_sdk/logger2.h>

namespace {

struct ObjectInfo {
    OBJECT_HANDLE handle{};
    int layer{};
    int start{};
    int end{};
    bool selected{};
};

struct RebuildOp {
    OBJECT_HANDLE handle{};
    int layer{};
    int new_start{};
    int new_end{};
    std::string alias;
};

struct MoveOp {
    OBJECT_HANDLE handle{};
    int layer{};
    int old_start{};
    int new_start{};
};

LOG_HANDLE* g_logger = nullptr;

struct Stopwatch {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};

    Stopwatch() {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }

    double elapsed_ms() const {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    }
};

void log_message(std::wstring const& message) {
    if (g_logger && g_logger->log) {
        g_logger->log(g_logger, message.c_str());
    }
}

void log_timing(std::wstring const& stage,
                double total_ms,
                double collect_ms,
                double enumerate_ms,
                double plan_ms,
                double delete_ms,
                double move_ms,
                double rebuild_ms,
                std::size_t selected_count,
                std::size_t object_count,
                std::size_t delete_count,
                std::size_t move_count,
                std::size_t rebuild_count) {
    std::wstring message = L"[timing] stage=" + stage;
    message += L" total=" + std::to_wstring(total_ms) + L"ms";
    message += L" collect=" + std::to_wstring(collect_ms) + L"ms";
    message += L" enumerate=" + std::to_wstring(enumerate_ms) + L"ms";
    message += L" plan=" + std::to_wstring(plan_ms) + L"ms";
    message += L" delete=" + std::to_wstring(delete_ms) + L"ms";
    message += L" move=" + std::to_wstring(move_ms) + L"ms";
    message += L" rebuild=" + std::to_wstring(rebuild_ms) + L"ms";
    message += L" selected=" + std::to_wstring(selected_count);
    message += L" objects=" + std::to_wstring(object_count);
    message += L" delete_count=" + std::to_wstring(delete_count);
    message += L" move_count=" + std::to_wstring(move_count);
    message += L" rebuild_count=" + std::to_wstring(rebuild_count);
    log_message(message);
}

COMMON_PLUGIN_TABLE g_common_plugin_table = {
    L"RippleTrim",
    L"TLリップル風に選択区間を削除し、重なったオブジェクトは分割せず短縮する",
};

std::vector<OBJECT_HANDLE> collect_selected_objects(EDIT_SECTION* edit) {
    std::vector<OBJECT_HANDLE> selected;
    int const selected_num = edit->get_selected_object_num();
    selected.reserve(selected_num > 0 ? selected_num : 1);
    for (int i = 0; i < selected_num; ++i) {
        if (OBJECT_HANDLE object = edit->get_selected_object(i)) {
            selected.push_back(object);
        }
    }
    if (selected.empty()) {
        if (OBJECT_HANDLE focus = edit->get_focus_object()) {
            selected.push_back(focus);
        }
    }
    return selected;
}

std::vector<ObjectInfo> enumerate_objects(EDIT_SECTION* edit,
                                          std::unordered_set<OBJECT_HANDLE> const& selected_set,
                                          int start_frame,
                                          int end_frame) {
    std::vector<ObjectInfo> objects;
    int const layer_max = std::max(edit->info->layer_max, 0);
    int const frame_max = std::max(edit->info->frame_max, 0);
    int const first_frame = std::clamp(start_frame, 0, frame_max);
    int const last_frame = std::clamp(end_frame, first_frame, frame_max);
    for (int layer = 0; layer <= layer_max; ++layer) {
        int frame = first_frame;
        while (frame <= last_frame) {
            OBJECT_HANDLE object = edit->find_object(layer, frame);
            if (!object) {
                ++frame;
                continue;
            }
            OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(object);
            if (lf.end < lf.start) {
                ++frame;
                continue;
            }
            if (lf.start > last_frame) {
                break;
            }
            objects.push_back(ObjectInfo{
                object,
                layer,
                lf.start,
                lf.end,
                selected_set.find(object) != selected_set.end(),
            });
            frame = std::max(frame + 1, lf.end + 1);
        }
    }
    return objects;
}

int compress_time(int t, int cut_start, int cut_end_exclusive) {
    if (t <= cut_start) {
        return t;
    }
    if (t >= cut_end_exclusive) {
        return t - (cut_end_exclusive - cut_start);
    }
    return cut_start;
}

bool replace_alias_frame(std::string& alias, int new_length) {
    if (new_length <= 0) {
        return false;
    }

    std::string const replacement = "frame=0," + std::to_string(new_length - 1);
    std::size_t object_header = alias.find("[Object]");
    if (object_header == std::string::npos) {
        return false;
    }

    std::size_t section_end = alias.find("\n[Object.", object_header);
    if (section_end == std::string::npos) {
        section_end = alias.size();
    }

    std::size_t frame_pos = alias.find("frame=", object_header);
    if (frame_pos != std::string::npos && frame_pos < section_end) {
        std::size_t frame_end = alias.find('\n', frame_pos);
        if (frame_end == std::string::npos || frame_end > section_end) {
            frame_end = section_end;
        }
        alias.replace(frame_pos, frame_end - frame_pos, replacement);
        return true;
    }

    std::size_t insert_pos = alias.find('\n', object_header);
    if (insert_pos == std::string::npos) {
        alias.append("\n");
        alias.append(replacement);
        alias.append("\n");
        return true;
    }

    alias.insert(insert_pos + 1, replacement + "\n");
    return true;
}

enum class PrepareResult {
    Unchanged,
    MoveOnly,
    Ready,
    Error,
};

enum class TrimMode {
    FullTimeline,
    SelectedRangeOnly,
};

PrepareResult prepare_rebuild_op(EDIT_SECTION* edit,
                                 ObjectInfo const& object,
                                 int cut_start,
                                 int cut_end_exclusive,
                                 RebuildOp& rebuild_op,
                                 MoveOp& move_op) {
    int const old_end_exclusive = object.end + 1;
    int const new_start = compress_time(object.start, cut_start, cut_end_exclusive);
    int const new_end_exclusive = compress_time(old_end_exclusive, cut_start, cut_end_exclusive);
    if (new_start == object.start && new_end_exclusive == old_end_exclusive) {
        return PrepareResult::Unchanged;
    }

    if ((new_end_exclusive - new_start) == (old_end_exclusive - object.start)) {
        move_op.handle = object.handle;
        move_op.layer = object.layer;
        move_op.old_start = object.start;
        move_op.new_start = new_start;
        return PrepareResult::MoveOnly;
    }

    rebuild_op.handle = object.handle;
    rebuild_op.layer = object.layer;
    rebuild_op.new_start = new_start;
    rebuild_op.new_end = new_end_exclusive - 1;

    if (new_end_exclusive <= new_start) {
        return PrepareResult::Ready;
    }

    LPCSTR alias_ptr = edit->get_object_alias(object.handle);
    if (!alias_ptr || alias_ptr[0] == '\0') {
        MessageBoxW(nullptr,
                    L"オブジェクトのエイリアス取得に失敗したため、リップル短縮を中断しました。",
                    L"RippleTrim",
                    MB_OK | MB_ICONERROR);
        return PrepareResult::Error;
    }

    rebuild_op.alias = alias_ptr;
    if (!replace_alias_frame(rebuild_op.alias, rebuild_op.new_end - rebuild_op.new_start + 1)) {
        MessageBoxW(nullptr,
                    L"オブジェクトのフレーム情報を書き換えられなかったため、リップル短縮を中断しました。",
                    L"RippleTrim",
                    MB_OK | MB_ICONERROR);
        rebuild_op.alias.clear();
        return PrepareResult::Error;
    }
    return PrepareResult::Ready;
}

void sort_rebuild_ops(std::vector<RebuildOp>& ops) {
    std::sort(ops.begin(), ops.end(), [](RebuildOp const& a, RebuildOp const& b) {
        if (a.layer != b.layer) {
            return a.layer < b.layer;
        }
        if (a.new_start != b.new_start) {
            return a.new_start < b.new_start;
        }
        return a.new_end < b.new_end;
    });
}

void sort_move_ops(std::vector<MoveOp>& ops) {
    std::sort(ops.begin(), ops.end(), [](MoveOp const& a, MoveOp const& b) {
        if (a.layer != b.layer) {
            return a.layer < b.layer;
        }
        if (a.new_start != b.new_start) {
            return a.new_start < b.new_start;
        }
        return a.old_start < b.old_start;
    });
}

void run_ripple_trim(EDIT_SECTION* edit, TrimMode mode) {
    if (!edit || !edit->info) {
        return;
    }

    Stopwatch total_timer{};
    double collect_ms = 0.0;
    double enumerate_ms = 0.0;
    double plan_ms = 0.0;
    double delete_ms = 0.0;
    double move_ms = 0.0;
    double rebuild_ms = 0.0;

    Stopwatch phase_timer{};
    std::vector<OBJECT_HANDLE> const selected = collect_selected_objects(edit);
    collect_ms = phase_timer.elapsed_ms();
    if (selected.empty()) {
        log_timing(L"no-selection",
                   total_timer.elapsed_ms(),
                   collect_ms,
                   enumerate_ms,
                   plan_ms,
                   delete_ms,
                   move_ms,
                   rebuild_ms,
                   selected.size(),
                   0,
                   0,
                   0,
                   0);
        MessageBoxW(nullptr,
                    L"選択中のオブジェクトがありません。",
                    L"RippleTrim",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::unordered_set<OBJECT_HANDLE> selected_set(selected.begin(), selected.end());

    int cut_start = INT_MAX;
    int cut_end = INT_MIN;
    for (OBJECT_HANDLE object : selected) {
        OBJECT_LAYER_FRAME const lf = edit->get_object_layer_frame(object);
        cut_start = std::min(cut_start, lf.start);
        cut_end = std::max(cut_end, lf.end);
    }
    if (cut_start > cut_end) {
        return;
    }

    int enumerate_end = std::max(edit->info->frame_max, cut_end);
    if (mode == TrimMode::SelectedRangeOnly) {
        int const range_start = edit->info->select_range_start;
        int const range_end = edit->info->select_range_end;
        if (range_start < 0 || range_end < 0 || range_end < range_start) {
            MessageBoxW(nullptr,
                        L"フレーム範囲選択を設定してから実行してください。",
                        L"RippleTrim",
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (range_end < cut_start) {
            MessageBoxW(nullptr,
                        L"フレーム範囲選択の終了位置が、選択オブジェクトより前にあります。",
                        L"RippleTrim",
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
        enumerate_end = range_end;
    }

    phase_timer = Stopwatch{};
    int const cut_end_exclusive = cut_end + 1;
    std::vector<ObjectInfo> const objects = enumerate_objects(edit, selected_set, cut_start, enumerate_end);
    enumerate_ms = phase_timer.elapsed_ms();
    std::vector<OBJECT_HANDLE> delete_handles = selected;
    std::vector<MoveOp> move_ops;
    std::vector<RebuildOp> rebuild_ops;

    phase_timer = Stopwatch{};
    for (ObjectInfo const& object : objects) {
        if (object.selected) {
            continue;
        }

        RebuildOp rebuild_op{};
        MoveOp move_op{};
        PrepareResult const result =
            prepare_rebuild_op(edit, object, cut_start, cut_end_exclusive, rebuild_op, move_op);
        if (result == PrepareResult::Error) {
            plan_ms = phase_timer.elapsed_ms();
            log_timing(mode == TrimMode::SelectedRangeOnly ? L"range-only-prepare-error" : L"prepare-error",
                       total_timer.elapsed_ms(),
                       collect_ms,
                       enumerate_ms,
                       plan_ms,
                       delete_ms,
                       move_ms,
                       rebuild_ms,
                       selected.size(),
                       objects.size(),
                       delete_handles.size(),
                       move_ops.size(),
                       rebuild_ops.size());
            return;
        }
        if (result == PrepareResult::Unchanged) {
            continue;
        }
        if (result == PrepareResult::MoveOnly) {
            move_ops.push_back(move_op);
            continue;
        }

        delete_handles.push_back(object.handle);
        if (!rebuild_op.alias.empty()) {
            rebuild_ops.push_back(std::move(rebuild_op));
        }
    }

    std::sort(delete_handles.begin(), delete_handles.end());
    delete_handles.erase(std::unique(delete_handles.begin(), delete_handles.end()), delete_handles.end());
    sort_move_ops(move_ops);
    sort_rebuild_ops(rebuild_ops);
    plan_ms = phase_timer.elapsed_ms();

    phase_timer = Stopwatch{};
    for (OBJECT_HANDLE object : delete_handles) {
        edit->delete_object(object);
    }
    delete_ms = phase_timer.elapsed_ms();

    // Free the cut range before moving later objects into it.
    phase_timer = Stopwatch{};
    for (MoveOp const& op : move_ops) {
        if (!edit->move_object(op.handle, op.layer, op.new_start)) {
            move_ms = phase_timer.elapsed_ms();
            log_timing(mode == TrimMode::SelectedRangeOnly ? L"range-only-move-error" : L"move-error",
                       total_timer.elapsed_ms(),
                       collect_ms,
                       enumerate_ms,
                       plan_ms,
                       delete_ms,
                       move_ms,
                       rebuild_ms,
                       selected.size(),
                       objects.size(),
                       delete_handles.size(),
                       move_ops.size(),
                       rebuild_ops.size());
            MessageBoxW(nullptr,
                        L"オブジェクトの移動に失敗しました。重なりなどで処理を継続できませんでした。",
                        L"RippleTrim",
                        MB_OK | MB_ICONERROR);
            return;
        }
    }
    move_ms = phase_timer.elapsed_ms();

    OBJECT_HANDLE first_created = nullptr;
    phase_timer = Stopwatch{};
    for (RebuildOp const& op : rebuild_ops) {
        OBJECT_HANDLE created = edit->create_object_from_alias(op.alias.c_str(), op.layer, op.new_start, 0);
        if (!created) {
            rebuild_ms = phase_timer.elapsed_ms();
            log_timing(mode == TrimMode::SelectedRangeOnly ? L"range-only-rebuild-error" : L"rebuild-error",
                       total_timer.elapsed_ms(),
                       collect_ms,
                       enumerate_ms,
                       plan_ms,
                       delete_ms,
                       move_ms,
                       rebuild_ms,
                       selected.size(),
                       objects.size(),
                       delete_handles.size(),
                       move_ops.size(),
                       rebuild_ops.size());
            MessageBoxW(nullptr,
                        L"オブジェクトの再生成に失敗しました。途中まで反映されている可能性があります。",
                        L"RippleTrim",
                        MB_OK | MB_ICONERROR);
            return;
        }
        if (!first_created) {
            first_created = created;
        }
    }
    rebuild_ms = phase_timer.elapsed_ms();

    if (first_created) {
        edit->set_focus_object(first_created);
    }

    log_timing(mode == TrimMode::SelectedRangeOnly ? L"range-only-success" : L"success",
               total_timer.elapsed_ms(),
               collect_ms,
               enumerate_ms,
               plan_ms,
               delete_ms,
               move_ms,
               rebuild_ms,
               selected.size(),
               objects.size(),
               delete_handles.size(),
               move_ops.size(),
               rebuild_ops.size());
}

void on_ripple_trim(EDIT_SECTION* edit) {
    run_ripple_trim(edit, TrimMode::FullTimeline);
}

void on_ripple_trim_selected_range_only(EDIT_SECTION* edit) {
    run_ripple_trim(edit, TrimMode::SelectedRangeOnly);
}

}  // namespace

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable() {
    return &g_common_plugin_table;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD version) {
    (void)version;
    return true;
}

EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* logger) {
    g_logger = logger;
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    if (!host) {
        return;
    }
    host->register_edit_menu(L"タイムライン\\リップル削除(重なり短縮)", on_ripple_trim);
    host->register_edit_menu(L"タイムライン\\リップル削除(重なり短縮/選択範囲のみ)", on_ripple_trim_selected_range_only);
    host->register_object_menu(L"リップル削除(重なり短縮/選択範囲のみ)", on_ripple_trim_selected_range_only);
    host->register_object_menu(L"リップル削除(重なり短縮)", on_ripple_trim);
}
