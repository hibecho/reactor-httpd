#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

// 未消费输入与已解析 HTTP 正文分别计量；这些额度不是进程 RSS 限制。
struct ResourceLimits
{
    std::size_t max_connections = 1024;
    std::size_t max_input = 256 * 1024;
    std::size_t output_low = 256 * 1024;
    std::size_t output_high = 1024 * 1024;
    std::size_t max_output = 16 * 1024 * 1024;
    std::size_t max_total_output = 64 * 1024 * 1024;

    void Validate() const
    {
        if (!max_connections || !max_input || !output_low ||
            output_low >= output_high || output_high > max_output ||
            max_output > max_total_output)
            throw std::invalid_argument("invalid connection/resource limits");
    }
};

// 服务器停止接受与所有连接的发送提交共用同步边界。
struct OutputBudget
{
    ResourceLimits limits;
    std::mutex mutex;
    std::atomic<bool> stopping{false};
    std::size_t used = 0;
};

struct SendAccount
{
    explicit SendAccount(std::shared_ptr<OutputBudget> value) : budget(std::move(value)) {}
    std::shared_ptr<OutputBudget> budget;
    std::size_t pending = 0; // 排队发送 + 输出 Buffer
    std::size_t queued_sends = 0;
    bool send_open = false;
    bool submit_open = true;
};

// 任务持有的额度凭证。转交输出 Buffer 前，任何异常/任务丢弃均自动归还。
struct SendReservation
{
    explicit SendReservation(std::shared_ptr<SendAccount> value) : account(std::move(value)) {}
    ~SendReservation()
    {
        if (bytes != 0 || queued)
        {
            std::lock_guard<std::mutex> lock(account->budget->mutex);
            if (queued) --account->queued_sends;
            account->pending -= bytes;
            account->budget->used -= bytes;
        }
    }
    std::shared_ptr<SendAccount> account;
    std::size_t bytes = 0;
    bool queued = false;
};
