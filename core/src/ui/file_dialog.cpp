// Small cross-platform file / folder browser built with ImGui.

#include <SharkSDR/paths.h>
#include <SharkSDR/ui/widgets.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace SharkSDR::ui {
namespace {

struct Entry {
    std::string name;  // UTF-8
    bool directory = false;
    uintmax_t size = 0;
};

struct BrowserState {
    fs::path dir;
    std::string dirText;
    std::string fileName;
    std::vector<Entry> entries;
    std::string error;
    std::vector<std::string> extensions;
    bool folderMode = false;
};

std::string toLower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> splitExtensions(const char* exts) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = exts ? exts : ""; *p; p++) {
        if (*p == ';' || *p == ',') {
            if (!cur.empty()) out.push_back(toLower(cur));
            cur.clear();
        } else if (*p != ' ' && *p != '*') {
            cur.push_back(*p);
        }
    }
    if (!cur.empty()) out.push_back(toLower(cur));
    return out;
}

void refresh(BrowserState& st) {
    st.entries.clear();
    st.error.clear();
    std::error_code ec;
    fs::directory_iterator it(st.dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        st.error = ec.message();
        return;
    }
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        Entry e;
        e.name = pathToUtf8(it->path().filename());
        if (e.name.empty() || e.name[0] == '.') continue;
        std::error_code ec2;
        e.directory = it->is_directory(ec2);
        if (!e.directory) {
            if (st.folderMode) continue;
            if (!st.extensions.empty()) {
                std::string ext = toLower(pathToUtf8(it->path().extension()));
                if (std::find(st.extensions.begin(), st.extensions.end(), ext) == st.extensions.end()) continue;
            }
            e.size = it->file_size(ec2);
        }
        st.entries.push_back(std::move(e));
    }
    std::sort(st.entries.begin(), st.entries.end(), [](const Entry& a, const Entry& b) {
        if (a.directory != b.directory) return a.directory;
        return toLower(a.name) < toLower(b.name);
    });
}

void changeDir(BrowserState& st, const fs::path& dir) {
    std::error_code ec;
    fs::path p = fs::absolute(dir, ec);
    if (ec || !fs::is_directory(p, ec)) return;
    st.dir = p.lexically_normal();
    st.dirText = pathToUtf8(st.dir);
    refresh(st);
}

std::string formatSize(uintmax_t s) {
    char buf[32];
    if (s >= (uintmax_t(1) << 30)) std::snprintf(buf, sizeof(buf), "%.1f GB", double(s) / double(1 << 30));
    else if (s >= (uintmax_t(1) << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", double(s) / double(1 << 20));
    else if (s >= 1024) std::snprintf(buf, sizeof(buf), "%.1f kB", double(s) / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%u B", unsigned(s));
    return buf;
}

// Returns true when the user confirmed a selection (written to *path).
bool browserPopup(const char* popupId, BrowserState& st, std::string* path) {
    bool result = false;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.6f, vp->WorkSize.y * 0.7f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_NoSavedSettings)) return false;

    // Navigation bar
    if (ImGui::Button("Up")) changeDir(st, st.dir.parent_path());
    ImGui::SameLine();
    if (ImGui::Button("Home")) changeDir(st, homeDirectory());
#if defined(_WIN32)
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
    std::string drive = pathToUtf8(st.dir.root_name());
    if (ImGui::BeginCombo("##drive", drive.c_str())) {
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; i++) {
            if (!(mask & (1u << i))) continue;
            std::string d = std::string(1, char('A' + i)) + ":";
            if (ImGui::Selectable(d.c_str(), d == drive)) changeDir(st, fs::path(d + "\\"));
        }
        ImGui::EndCombo();
    }
#endif
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##dir", &st.dirText, ImGuiInputTextFlags_EnterReturnsTrue)) changeDir(st, utf8ToPath(st.dirText));

    // Listing
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f;
    if (ImGui::BeginChild("##list", ImVec2(0, -footer), ImGuiChildFlags_Borders)) {
        if (!st.error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", st.error.c_str());
        fs::path enterDir;
        bool doubleClickedFile = false;
        if (ImGui::BeginTable("##files", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
            for (const Entry& e : st.entries) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                std::string label = (e.directory ? "[+] " : "    ") + e.name;
                bool selected = !e.directory && e.name == st.fileName;
                if (ImGui::Selectable(label.c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (e.directory) {
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) enterDir = st.dir / utf8ToPath(e.name);
                    } else {
                        st.fileName = e.name;
                        doubleClickedFile = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                    }
                }
                ImGui::TableNextColumn();
                if (!e.directory) ImGui::TextDisabled("%s", formatSize(e.size).c_str());
            }
            ImGui::EndTable();
        }
        if (!enterDir.empty()) changeDir(st, enterDir);
        if (doubleClickedFile) {
            *path = pathToUtf8(st.dir / utf8ToPath(st.fileName));
            result = true;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();

    // Footer
    if (!st.folderMode) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("File");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##filename", &st.fileName);
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Select the current folder");
    }
    const float bw = ImGui::GetFontSize() * 6.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
    const bool canOk = st.folderMode || !st.fileName.empty();
    ImGui::BeginDisabled(!canOk);
    if (ImGui::Button("OK", ImVec2(bw, 0))) {
        *path = pathToUtf8(st.folderMode ? st.dir : st.dir / utf8ToPath(st.fileName));
        result = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return result;
}

}  // namespace

bool PathPicker(const char* id, std::string* path, bool selectFolder, const char* extensions) {
    static std::unordered_map<ImGuiID, BrowserState> states;
    ImGui::PushID(id);
    bool changed = false;

    const float buttonW = ImGui::GetFrameHeight() * 1.5f;
    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - buttonW - ImGui::GetStyle().ItemInnerSpacing.x));
    if (ImGui::InputText("##path", path)) changed = true;
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);

    const char* popupId = selectFolder ? "Select folder" : "Open file";
    BrowserState& st = states[ImGui::GetID("##browser")];
    if (ImGui::Button("...", ImVec2(buttonW, 0))) {
        st.folderMode = selectFolder;
        st.extensions = splitExtensions(extensions);
        fs::path start = utf8ToPath(*path);
        std::error_code ec;
        st.fileName.clear();
        if (!path->empty() && fs::is_regular_file(start, ec)) {
            st.fileName = pathToUtf8(start.filename());
            start = start.parent_path();
        }
        if (path->empty() || !fs::is_directory(start, ec)) start = homeDirectory();
        changeDir(st, start);
        ImGui::OpenPopup(popupId);
    }
    if (browserPopup(popupId, st, path)) changed = true;
    ImGui::PopID();
    return changed;
}

}  // namespace SharkSDR::ui
