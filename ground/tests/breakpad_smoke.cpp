// Exercise the pinned Linux client's public API and real signal handling.
#include <client/linux/handler/exception_handler.h>
#include <client/linux/handler/minidump_descriptor.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

static bool dumpWritten(const google_breakpad::MinidumpDescriptor &descriptor,
                        void *context, bool succeeded)
{
    const int resultFd = *static_cast<int *>(context);
    const char result = succeeded ? '1' : '0';
    const ssize_t resultWritten = write(resultFd, &result, 1);
    (void)resultWritten;
    if (succeeded) {
        // A crash callback must not allocate or use Qt/stdio.
        const char *path = descriptor.path();
        const ssize_t pathResult = write(STDOUT_FILENO, path, std::strlen(path));
        const ssize_t newlineResult = write(STDOUT_FILENO, "\n", 1);
        (void)pathResult;
        (void)newlineResult;
    }
    return succeeded;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2; // An existing, isolated dump directory is required.

    int resultPipe[2];
    if (pipe(resultPipe) < 0)
        return 3;
    const pid_t child = fork();
    if (child < 0)
        return 3;
    if (child == 0) {
        close(resultPipe[0]);
        google_breakpad::ExceptionHandler handler(
            google_breakpad::MinidumpDescriptor(argv[1]), nullptr,
            dumpWritten, &resultPipe[1], true, -1);
        raise(SIGSEGV);
        _exit(4); // The signal must terminate the child.
    }

    close(resultPipe[1]);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    char result = '0';
    ssize_t bytesRead;
    do {
        bytesRead = read(resultPipe[0], &result, 1);
    } while (bytesRead < 0 && errno == EINTR);
    close(resultPipe[0]);
    return bytesRead == 1 && result == '1' && waited == child
                   && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV
               ? 0 : 5;
}
