#include "interop.hpp"
#include "extensions.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>
namespace vulkano {
int duplicateSyncFd(int fd) {
    require(fd >= -1, "Invalid sync file descriptor");
    if (fd == -1)
        return -1;
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0)
        throw std::system_error(errno, std::generic_category(), "duplicate sync fd");
    return copy;
}
ExternalSemaphore::ExternalSemaphore(std::shared_ptr<Device> device, int importFd) : Resource(std::move(device)) {
    require(d->enabledExtra & ExternalSyncFd, "External SYNC_FD feature was not enabled");
    require(importFd >= -2, "Invalid external semaphore descriptor");
    VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (importFd == -2)
        create.pNext = &exportInfo;
    check(vkCreateSemaphore(d->device, &create, nullptr, &semaphore), "create external semaphore");
    int copy = -1;
    try {
        if (importFd != -2) {
            copy = duplicateSyncFd(importFd);
            VkImportSemaphoreFdInfoKHR info{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
            info.semaphore = semaphore;
            info.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
            info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            info.fd = copy;
            check(d->extensions->importSemaphoreFd(d->device, &info), "import sync fd");
            copy = -1; // Vulkan owns the duplicate after successful import.
            state = State::Imported;
        }
    } catch (...) {
        if (copy >= 0)
            close(copy);
        vkDestroySemaphore(d->device, semaphore, nullptr);
        semaphore = VK_NULL_HANDLE;
        throw;
    }
}
int ExternalSemaphore::exportFd() {
    require(state == State::Signalled, "Submit a signal before exporting this one-shot semaphore");
    VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    info.semaphore = semaphore;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int fd;
    check(d->extensions->getSemaphoreFd(d->device, &info, &fd), "export sync fd");
    state = State::Exported;
    return fd;
}
ExternalSemaphore::~ExternalSemaphore() {
    if (semaphore)
        vkDestroySemaphore(d->device, semaphore, nullptr);
}
void Command::waitExternal(std::shared_ptr<ExternalSemaphore> event) {
    recording();
    require(event && event->owner() == d.get(), "External semaphore device mismatch");
    require(std::find(externalWaits.begin(), externalWaits.end(), event) == externalWaits.end(),
            "Duplicate external wait");
    externalWaits.push_back(std::move(event));
}
void Command::signalExternal(std::shared_ptr<ExternalSemaphore> event) {
    recording();
    require(event && event->owner() == d.get(), "External semaphore device mismatch");
    require(std::find(externalSignals.begin(), externalSignals.end(), event) == externalSignals.end(),
            "Duplicate external signal");
    externalSignals.push_back(std::move(event));
}
} // namespace vulkano
