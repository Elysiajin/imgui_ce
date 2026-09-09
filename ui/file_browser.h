#ifndef FILE_BROWSER_H
#define FILE_BROWSER_H

#include <filesystem>
#include <vector>
#include <string>
#include <Imgui/imgui.h>

// fs alias used inside this translation unit only (kept short on Windows).
namespace fs = std::filesystem;

struct file_item {
    fs::path path;
    std::string display_name;
    std::intmax_t size = 0;
    std::string last_write_time;
    bool is_directory = false;
    bool is_hidden = false;
    bool is_readonly = false;
};

class file_browser
{
public:
    file_browser();

    void render();

    const fs::path& current_path() const { return current_path_; }
    bool has_selection() const { return !selected_path_.empty(); }
    fs::path selected_path() const { return selected_path_; }
    void set_initial_path(const fs::path& p);

    void clear_filter();
    void set_filter(const std::string& ext);

private:
    void refresh();
    bool navigate(const fs::path& dir);

    fs::path current_path_;
    fs::path selected_path_;
    std::vector<file_item> items_;

    std::string filter_;
    char filter_buf_[128]{};
    bool show_hidden_ = false;
};

#endif // FILE_BROWSER_H