#pragma once

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#include <windows.h>

class LogLine
{
public:
    LogLine()
        : m_lock(s_mutex)
    {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch())
                      .count() %
                  1000;

        struct tm tm_buf{};
        localtime_s(&tm_buf, &t);

        std::ostringstream prefix;
        prefix << std::put_time(&tm_buf, "[%y-%m-%d %H:%M:%S.")
               << std::setfill('0') << std::setw(3) << ms << "] ";

        EnsureFileOpen(tm_buf, ms);

        std::cout << prefix.str();
        s_file << prefix.str();
    }

    ~LogLine() // or we lost these logs in buf
    {
        s_file.flush();
    }

    template <typename T>
    LogLine &operator<<(const T &val)
    {
        std::cout << val;
        s_file << val;
        return *this;
    }

    LogLine &operator<<(std::ostream &(*manip)(std::ostream &))
    {
        std::cout << manip;
        s_file << manip;
        return *this;
    }

private:
    static void EnsureFileOpen(const struct tm &tm_buf, const long long &ms)
    {
        if (s_file.is_open())
            return;
        CreateDirectoryA("logs", NULL);
        std::ostringstream name;
        name << "logs\\"
             << std::put_time(&tm_buf, "SteamP2PTest-%Y-%m-%d_%H-%M-%S.")
             << std::setfill('0') << std::setw(3) << ms
             << ".log";
        s_file.open(name.str(), std::ios::out | std::ios::app);
    }

    static std::mutex s_mutex;
    static std::ofstream s_file;

    std::unique_lock<std::mutex> m_lock;
};

inline std::mutex LogLine::s_mutex;
inline std::ofstream LogLine::s_file;

#define Log() LogLine()
