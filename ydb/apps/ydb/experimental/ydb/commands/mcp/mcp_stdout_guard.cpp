#include "mcp_stdout_guard.h"

#include <util/stream/output.h>

#if defined(_unix_)
#include <unistd.h>
#else
#include <io.h>
#endif

namespace NYdb::NConsoleClient::NMcp {

namespace {

constexpr int STDOUT_FD = 1;
constexpr int STDERR_FD = 2;

int DupFd(int fd) {
#if defined(_unix_)
    return ::dup(fd);
#else
    return ::_dup(fd);
#endif
}

void Dup2Fd(int oldFd, int newFd) {
#if defined(_unix_)
    ::dup2(oldFd, newFd);
#else
    ::_dup2(oldFd, newFd);
#endif
}

void CloseFd(int fd) {
#if defined(_unix_)
    ::close(fd);
#else
    ::_close(fd);
#endif
}

int RawWrite(int fd, const void* buffer, size_t count) {
#if defined(_unix_)
    return static_cast<int>(::write(fd, buffer, count));
#else
    return ::_write(fd, buffer, static_cast<unsigned int>(count));
#endif
}

} // anonymous namespace

TProtocolStdoutGuard::TProtocolStdoutGuard() {
    Cout.Flush();
    ProtocolFd_ = DupFd(STDOUT_FD);
    if (ProtocolFd_ >= 0) {
        Dup2Fd(STDERR_FD, STDOUT_FD);
    }
}

TProtocolStdoutGuard::~TProtocolStdoutGuard() {
    if (ProtocolFd_ < 0) {
        return;
    }
    Cout.Flush();
    Dup2Fd(ProtocolFd_, STDOUT_FD);
    CloseFd(ProtocolFd_);
    ProtocolFd_ = -1;
}

bool TProtocolStdoutGuard::WriteToRealStdout(TStringBuf data) {
    if (ProtocolFd_ < 0) {
        return false;
    }
    while (!data.empty()) {
        const int written = RawWrite(ProtocolFd_, data.data(), data.size());
        if (written <= 0) {
            return false;
        }
        data.Skip(static_cast<size_t>(written));
    }
    return true;
}

} // namespace NYdb::NConsoleClient::NMcp
