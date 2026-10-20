#include "arec/execution_engine.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#include <utility>

#include <Windows.h>
#include <winternl.h>

#include "arec/crypto.hpp"
#include "arec/token.hpp"

namespace arec {
namespace {

std::string utf16_to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        output.data(),
        size,
        nullptr,
        nullptr);
    return output;
}

std::wstring utf8_to_utf16(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (size <= 0) {
        throw std::invalid_argument("value is not valid UTF-8");
    }
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        output.data(),
        size);
    return output;
}

std::string console_bytes_to_utf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_OEMCP,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (size <= 0) {
        return value;
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_OEMCP,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        wide.data(),
        size);
    return utf16_to_utf8(wide);
}

bool is_valid_utf8(const std::string& value) {
    if (value.empty()) {
        return true;
    }
    return MultiByteToWideChar(
               CP_UTF8,
               MB_ERR_INVALID_CHARS,
               value.data(),
               static_cast<int>(value.size()),
               nullptr,
               0) > 0;
}

ToolResponse failure(
    const std::string& code,
    const std::string& message,
    bool retryable = false,
    const std::string& platform = "windows") {
    ToolResponse response;
    response.status = Status::failed;
    response.error_code = code;
    response.retryable = retryable;
    response.message = message;
    response.executed_at = utc_now_iso8601();
    response.platform = platform;
    return response;
}

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle)
        : handle_(handle) {}

    ~UniqueHandle() {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const {
        return handle_;
    }

    HANDLE release() {
        HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }

    void reset(HANDLE value = nullptr) {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
        handle_ = value;
    }

    explicit operator bool() const {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = nullptr;
};

struct CommandExecution {
    DWORD exit_code = 1;
    bool timed_out = false;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    std::string stdout_bytes;
    std::string stderr_bytes;
    std::string error;
};

void read_pipe(
    HANDLE pipe,
    std::size_t cap,
    std::string& output,
    bool& truncated) {
    std::array<char, 4096> buffer{};
    DWORD bytes_read = 0;
    while (ReadFile(
        pipe,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &bytes_read,
        nullptr)) {
        if (bytes_read == 0) {
            break;
        }
        const std::size_t available = cap > output.size() ? cap - output.size() : 0U;
        if (available != 0U) {
            output.append(buffer.data(), std::min<std::size_t>(available, bytes_read));
        }
        if (static_cast<std::size_t>(bytes_read) > available) {
            truncated = true;
        }
    }
}

CommandExecution run_command(
    const std::string& command,
    const std::string& working_directory,
    std::int64_t deadline_ms) {
    CommandExecution execution;

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE stdout_read_raw = nullptr;
    HANDLE stdout_write_raw = nullptr;
    HANDLE stderr_read_raw = nullptr;
    HANDLE stderr_write_raw = nullptr;

    if (!CreatePipe(&stdout_read_raw, &stdout_write_raw, &security, 0) ||
        !CreatePipe(&stderr_read_raw, &stderr_write_raw, &security, 0)) {
        if (stdout_read_raw != nullptr) {
            CloseHandle(stdout_read_raw);
        }
        if (stdout_write_raw != nullptr) {
            CloseHandle(stdout_write_raw);
        }
        if (stderr_read_raw != nullptr) {
            CloseHandle(stderr_read_raw);
        }
        if (stderr_write_raw != nullptr) {
            CloseHandle(stderr_write_raw);
        }
        execution.error = "unable to create output pipes";
        return execution;
    }

    UniqueHandle stdout_read(stdout_read_raw);
    UniqueHandle stdout_write(stdout_write_raw);
    UniqueHandle stderr_read(stderr_read_raw);
    UniqueHandle stderr_write(stderr_write_raw);

    SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = stdout_write.get();
    startup.hStdError = stderr_write.get();
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process_info{};

    wchar_t command_processor[MAX_PATH]{};
    const DWORD command_processor_size =
        GetEnvironmentVariableW(L"COMSPEC", command_processor, MAX_PATH);
    std::wstring executable =
        command_processor_size > 0 && command_processor_size < MAX_PATH
            ? std::wstring(command_processor, command_processor_size)
            : std::wstring(L"C:\\Windows\\System32\\cmd.exe");

    std::wstring command_line = L"\"" + executable + L"\" /d /s /c \"" +
        utf8_to_utf16(command) + L"\"";
    std::wstring working_directory_wide;
    LPCWSTR working_directory_pointer = nullptr;
    if (!working_directory.empty()) {
        working_directory_wide = utf8_to_utf16(working_directory);
        working_directory_pointer = working_directory_wide.c_str();
    }

    if (!CreateProcessW(
            executable.c_str(),
            command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW,
            nullptr,
            working_directory_pointer,
            &startup,
            &process_info)) {
        execution.error = "unable to start command process";
        return execution;
    }

    UniqueHandle process(process_info.hProcess);
    UniqueHandle thread(process_info.hThread);
    stdout_write.reset();
    stderr_write.reset();

    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        TerminateProcess(process.get(), 1);
        execution.error = "unable to create command job object";
        return execution;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
    job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job.get(),
            JobObjectExtendedLimitInformation,
            &job_limits,
            sizeof(job_limits)) ||
        !AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 1);
        execution.error = "unable to isolate command process";
        return execution;
    }

    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        TerminateJobObject(job.get(), 1);
        execution.error = "unable to resume command process";
        return execution;
    }

    std::thread stdout_reader(
        read_pipe,
        stdout_read.get(),
        kMaxToolOutputBytes,
        std::ref(execution.stdout_bytes),
        std::ref(execution.stdout_truncated));
    std::thread stderr_reader(
        read_pipe,
        stderr_read.get(),
        kMaxToolOutputBytes,
        std::ref(execution.stderr_bytes),
        std::ref(execution.stderr_truncated));

    const DWORD wait_result = WaitForSingleObject(
        process.get(),
        static_cast<DWORD>(std::max<std::int64_t>(1, deadline_ms)));
    if (wait_result == WAIT_TIMEOUT) {
        execution.timed_out = true;
        TerminateJobObject(job.get(), 1);
        WaitForSingleObject(process.get(), 5000);
    } else if (wait_result == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.get(), &execution.exit_code);
    } else {
        execution.error = "unable to wait for command process";
        TerminateJobObject(job.get(), 1);
    }

    stdout_reader.join();
    stderr_reader.join();
    return execution;
}

}  // namespace

ExecutionEngine::ExecutionEngine(
    std::string hmac_secret,
    std::string platform,
    std::size_t cache_capacity,
    std::chrono::milliseconds cache_ttl)
    : hmac_secret_(std::move(hmac_secret)),
      platform_(std::move(platform)),
      cache_capacity_(cache_capacity),
      cache_ttl_(cache_ttl) {
    if (hmac_secret_.empty()) {
        throw std::invalid_argument("HMAC secret must not be empty");
    }
    if (cache_capacity_ == 0U) {
        throw std::invalid_argument("cache capacity must be positive");
    }
    if (cache_ttl_.count() <= 0) {
        throw std::invalid_argument("cache TTL must be positive");
    }
}

nlohmann::json ExecutionEngine::tool_definitions() {
    const std::vector<ToolDefinition> tools = {
        {
            "system_info",
            "Return hostname, operating system and architecture",
            {"darwin", "windows"},
            true,
            {{"type", "object"}, {"properties", nlohmann::json::object()}},
        },
        {
            "exec_command",
            "Execute a shell command and return stdout, stderr and exit code",
            {"darwin", "windows"},
            false,
            {
                {"type", "object"},
                {"required", {"command"}},
                {
                    "properties",
                    {
                        {"command", {{"type", "string"}, {"minLength", 1}, {"maxLength", 8192}}},
                        {"cwd", {{"type", "string"}}},
                    },
                },
            },
        },
        {
            "read_file",
            "Read a UTF-8 file and truncate content at the configured limit",
            {"darwin", "windows"},
            true,
            {
                {"type", "object"},
                {"required", {"path"}},
                {
                    "properties",
                    {
                        {"path", {{"type", "string"}, {"minLength", 1}}},
                        {
                            "max_bytes",
                            {
                                {"type", "integer"},
                                {"minimum", 1},
                                {"maximum", kMaxToolOutputBytes},
                            },
                        },
                    },
                },
            },
        },
    };

    nlohmann::json result = nlohmann::json::array();
    for (const auto& tool : tools) {
        result.push_back(tool.to_json());
    }
    return result;
}

ToolResponse ExecutionEngine::handle_request(const nlohmann::json& request) {
    try {
        if (!request.is_object()) {
            return failure("PROTOCOL_ERROR", "request must be a JSON object", false, platform_);
        }
        if (request.value("protocol_version", "") != kProtocolVersion) {
            return failure("PROTOCOL_ERROR", "unsupported protocol version", false, platform_);
        }

        const std::string request_id = request.at("request_id").get<std::string>();
        const std::string session_id = request.at("session_id").get<std::string>();
        const std::string tool = request.at("tool").get<std::string>();
        const auto params = request.at("params");
        const std::string idempotency_key = request.at("idempotency_key").get<std::string>();
        const std::string token = request.at("token").get<std::string>();
        const std::int64_t deadline_ms =
            request.value("deadline_ms", kDefaultDeadlineMs);

        if (request_id.empty() || request_id.size() > 128U ||
            idempotency_key.empty() || idempotency_key.size() > 256U) {
            return failure("INVALID_PARAMS", "request identity is invalid", false, platform_);
        }
        if (deadline_ms <= 0 || deadline_ms > kMaxDeadlineMs) {
            return failure("INVALID_PARAMS", "deadline_ms is outside the allowed range", false, platform_);
        }
        if (!params.is_object()) {
            return failure("INVALID_PARAMS", "params must be a JSON object", false, platform_);
        }

        const auto validation = validate_capability_token(
            token,
            hmac_secret_,
            session_id,
            tool,
            unix_time_seconds());
        if (!validation.ok) {
            return failure("UNAUTHORIZED", "capability token was rejected", false, platform_);
        }

        const std::string params_hash = sha256_hex(tool + "\n" + params.dump());
        std::unique_lock<std::mutex> lock(cache_mutex_);
        const auto now = std::chrono::steady_clock::now();

        for (auto iterator = idempotency_entries_.begin();
             iterator != idempotency_entries_.end();) {
            if (iterator->second.expires_at <= now) {
                iterator = idempotency_entries_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (auto iterator = request_states_.begin(); iterator != request_states_.end();) {
            if (iterator->second.expires_at <= now) {
                iterator = request_states_.erase(iterator);
            } else {
                ++iterator;
            }
        }

        auto cached = idempotency_entries_.find(idempotency_key);
        if (cached != idempotency_entries_.end()) {
            if (cached->second.params_hash != params_hash) {
                return failure(
                    "IDEMPOTENCY_CONFLICT",
                    "idempotency key was reused with different parameters",
                    false,
                    platform_);
            }
            cache_cv_.wait(
                lock,
                [this, &idempotency_key] {
                    const auto iterator = idempotency_entries_.find(idempotency_key);
                    return iterator == idempotency_entries_.end() ||
                        iterator->second.complete;
                });
            cached = idempotency_entries_.find(idempotency_key);
            if (cached == idempotency_entries_.end()) {
                return failure(
                    "EXECUTION_FAILED",
                    "idempotency state became unavailable",
                    false,
                    platform_);
            }
            const ToolResponse response = cached->second.response;
            store_request_state(request_id, response, now);
            return response;
        }

        while (idempotency_entries_.size() >= cache_capacity_) {
            bool evicted = false;
            for (std::size_t attempts = 0;
                 attempts < idempotency_order_.size();
                 ++attempts) {
                const std::string oldest = idempotency_order_.front();
                idempotency_order_.pop_front();
                const auto iterator = idempotency_entries_.find(oldest);
                if (iterator == idempotency_entries_.end()) {
                    evicted = true;
                    break;
                }
                if (!iterator->second.complete) {
                    idempotency_order_.push_back(oldest);
                    continue;
                }
                idempotency_entries_.erase(iterator);
                evicted = true;
                break;
            }
            if (!evicted) {
                break;
            }
        }

        IdempotencyEntry entry;
        entry.params_hash = params_hash;
        entry.expires_at = now + cache_ttl_;
        idempotency_entries_.emplace(idempotency_key, std::move(entry));
        idempotency_order_.erase(
            std::remove(
                idempotency_order_.begin(),
                idempotency_order_.end(),
                idempotency_key),
            idempotency_order_.end());
        idempotency_order_.push_back(idempotency_key);

        ToolResponse running;
        running.status = Status::running;
        running.platform = platform_;
        running.executed_at = utc_now_iso8601();
        store_request_state(request_id, running, now);
        lock.unlock();

        ToolResponse response;
        try {
            response = execute_tool(tool, params, deadline_ms);
        } catch (const std::invalid_argument& exception) {
            response = failure("INVALID_PARAMS", exception.what(), false, platform_);
        } catch (const std::exception&) {
            response = failure("EXECUTION_FAILED", "tool execution failed", false, platform_);
        }

        lock.lock();
        auto completed = idempotency_entries_.find(idempotency_key);
        if (completed != idempotency_entries_.end()) {
            completed->second.complete = true;
            completed->second.response = response;
            completed->second.expires_at =
                std::chrono::steady_clock::now() + cache_ttl_;
        }
        store_request_state(request_id, response, std::chrono::steady_clock::now());
        lock.unlock();
        cache_cv_.notify_all();
        return response;
    } catch (const nlohmann::json::exception&) {
        return failure("INVALID_PARAMS", "request fields are missing or invalid", false, platform_);
    } catch (const std::exception&) {
        return failure("EXECUTION_FAILED", "request processing failed", false, platform_);
    }
}

nlohmann::json ExecutionEngine::handle_query(const nlohmann::json& query) {
    try {
        const std::string request_id = query.at("request_id").get<std::string>();
        if (request_id.empty() || request_id.size() > 128U) {
            throw std::invalid_argument("request_id is invalid");
        }

        std::lock_guard<std::mutex> lock(cache_mutex_);
        const auto now = std::chrono::steady_clock::now();
        const auto iterator = request_states_.find(request_id);
        ToolResponse response;
        if (iterator == request_states_.end() || iterator->second.expires_at <= now) {
            response.status = Status::unknown;
            response.error_code = "REQUEST_STATE_UNAVAILABLE";
            response.message = "request state is unavailable or has expired";
            response.retryable = false;
            response.platform = platform_;
            response.executed_at = utc_now_iso8601();
        } else {
            response = iterator->second.response;
        }

        auto value = response.to_json();
        value["type"] = "query_response";
        value["protocol_version"] = kProtocolVersion;
        value["request_id"] = request_id;
        return value;
    } catch (const std::exception&) {
        return {
            {"type", "query_response"},
            {"protocol_version", kProtocolVersion},
            {"request_id", ""},
            {"status", "unknown"},
            {"error_code", "INVALID_PARAMS"},
            {"retryable", false},
            {"message", "query fields are missing or invalid"},
            {"platform", platform_},
        };
    }
}

ToolResponse ExecutionEngine::execute_tool(
    const std::string& tool,
    const nlohmann::json& params,
    std::int64_t deadline_ms) {
    if (tool == "system_info") {
        return execute_system_info();
    }
    if (tool == "read_file") {
        return execute_read_file(params);
    }
    if (tool == "exec_command") {
        return execute_command(params, deadline_ms);
    }
    return failure("TOOL_NOT_FOUND", "tool is not registered", false, platform_);
}

ToolResponse ExecutionEngine::execute_system_info() {
    wchar_t hostname_buffer[256]{};
    DWORD hostname_size = static_cast<DWORD>(std::size(hostname_buffer));
    std::string hostname;
    if (GetComputerNameW(hostname_buffer, &hostname_size)) {
        hostname = utf16_to_utf8(
            std::wstring(hostname_buffer, hostname_size));
    }

    SYSTEM_INFO system_info{};
    GetNativeSystemInfo(&system_info);
    std::string architecture = "unknown";
    switch (system_info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64:
            architecture = "x86_64";
            break;
        case PROCESSOR_ARCHITECTURE_ARM64:
            architecture = "arm64";
            break;
        case PROCESSOR_ARCHITECTURE_INTEL:
            architecture = "x86";
            break;
        default:
            break;
    }

    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
        const auto rtl_get_version = reinterpret_cast<RtlGetVersionFunction>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtl_get_version != nullptr) {
            rtl_get_version(&version);
        }
    }

    std::ostringstream os;
    os << "Windows " << version.dwMajorVersion << "." << version.dwMinorVersion
       << "." << version.dwBuildNumber;

    ToolResponse response;
    response.status = Status::ok;
    response.platform = platform_;
    response.executed_at = utc_now_iso8601();
    response.result = {
        {"hostname", hostname},
        {"os", os.str()},
        {"arch", architecture},
    };
    return response;
}

ToolResponse ExecutionEngine::execute_read_file(const nlohmann::json& params) {
    const std::string path = params.at("path").get<std::string>();
    if (path.empty() || path.size() > 32768U) {
        throw std::invalid_argument("path is invalid");
    }

    std::size_t max_bytes = kMaxToolOutputBytes;
    if (params.contains("max_bytes")) {
        const auto requested = params.at("max_bytes").get<std::int64_t>();
        if (requested <= 0 ||
            requested > static_cast<std::int64_t>(kMaxToolOutputBytes)) {
            throw std::invalid_argument("max_bytes is outside the allowed range");
        }
        max_bytes = static_cast<std::size_t>(requested);
    }

    std::error_code error;
    const auto file_size = std::filesystem::file_size(
        std::filesystem::u8path(path),
        error);
    if (error || !std::filesystem::is_regular_file(std::filesystem::u8path(path), error)) {
        return failure("EXECUTION_FAILED", "file cannot be read", false, platform_);
    }

    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input) {
        return failure("EXECUTION_FAILED", "file cannot be opened", false, platform_);
    }

    std::string content(max_bytes, '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    const auto bytes_read = static_cast<std::size_t>(input.gcount());
    content.resize(bytes_read);

    if (!is_valid_utf8(content)) {
        return failure(
            "EXECUTION_FAILED",
            "file is not valid UTF-8",
            false,
            platform_);
    }

    ToolResponse response;
    response.status = Status::ok;
    response.platform = platform_;
    response.executed_at = utc_now_iso8601();
    response.result = {
        {"path", path},
        {"content", content},
        {"bytes_read", bytes_read},
        {"truncated", file_size > bytes_read},
    };
    return response;
}

ToolResponse ExecutionEngine::execute_command(
    const nlohmann::json& params,
    std::int64_t deadline_ms) {
    const std::string command = params.at("command").get<std::string>();
    if (command.empty() || command.size() > 8192U) {
        throw std::invalid_argument("command is invalid");
    }

    std::string working_directory;
    if (params.contains("cwd")) {
        working_directory = params.at("cwd").get<std::string>();
        if (working_directory.size() > 32768U) {
            throw std::invalid_argument("cwd is invalid");
        }
    }

    const CommandExecution execution = run_command(
        command,
        working_directory,
        deadline_ms);
    if (execution.timed_out) {
        ToolResponse response;
        response.status = Status::timeout;
        response.error_code = "EXECUTION_TIMEOUT";
        response.retryable = false;
        response.message = "deadline exceeded; execution state unknown";
        response.platform = platform_;
        response.executed_at = utc_now_iso8601();
        return response;
    }
    if (!execution.error.empty()) {
        return failure("EXECUTION_FAILED", execution.error, false, platform_);
    }

    ToolResponse response;
    response.status = Status::ok;
    response.platform = platform_;
    response.executed_at = utc_now_iso8601();
    response.result = {
        {"stdout", console_bytes_to_utf8(execution.stdout_bytes)},
        {"stderr", console_bytes_to_utf8(execution.stderr_bytes)},
        {"exit_code", static_cast<std::int64_t>(execution.exit_code)},
        {"stdout_truncated", execution.stdout_truncated},
        {"stderr_truncated", execution.stderr_truncated},
    };
    return response;
}

void ExecutionEngine::store_request_state(
    const std::string& request_id,
    const ToolResponse& response,
    std::chrono::steady_clock::time_point now) {
    while (request_states_.size() >= cache_capacity_) {
        bool evicted = false;
        for (std::size_t attempts = 0; attempts < request_order_.size(); ++attempts) {
            const std::string oldest = request_order_.front();
            request_order_.pop_front();
            const auto iterator = request_states_.find(oldest);
            if (iterator == request_states_.end() ||
                iterator->second.expires_at <= now) {
                if (iterator != request_states_.end()) {
                    request_states_.erase(iterator);
                }
                evicted = true;
                break;
            }
            request_order_.push_back(oldest);
        }
        if (!evicted) {
            break;
        }
    }
    request_order_.erase(
        std::remove(
            request_order_.begin(),
            request_order_.end(),
            request_id),
        request_order_.end());
    request_states_[request_id] = {response, now + cache_ttl_};
    request_order_.push_back(request_id);
}

}  // namespace arec
