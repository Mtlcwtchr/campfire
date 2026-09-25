#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace engine {
// Publication is independent of scheduling. A cancelled job may finish, but
// cannot replace a newer result. No callbacks execute under the publication lock.
template<class T> class Publication {
    struct Identity {};
public:
    class Ticket {
        friend class Publication;
        std::shared_ptr<const Identity> owner_;
        std::uint64_t version_ = 0;
        Ticket(std::shared_ptr<const Identity> owner, std::uint64_t version)
            : owner_(std::move(owner)), version_(version) {}
    public:
        std::uint64_t version() const { return version_; }
    };
    struct Snapshot {
        std::uint64_t version = 0;
        std::shared_ptr<const T> value;
    };
    Ticket request() {
        std::lock_guard lock(mutex_);
        return Ticket(identity_, ++requested_);
    }
    void cancel() { (void)request(); }
    bool current(Ticket ticket) const {
        std::lock_guard lock(mutex_);
        return currentLocked(ticket);
    }
    bool publish(Ticket ticket, std::shared_ptr<const T> value) {
        if (!value) return false;
        // Destroy retired data outside the lock (destructors may join workers).
        Snapshot next{ticket.version_, std::move(value)};
        {
            std::lock_guard lock(mutex_);
            if (!currentLocked(ticket)) return false;
            std::swap(published_, next);
        }
        return true;
    }
    Snapshot read() const {
        std::lock_guard lock(mutex_);
        return published_;
    }
private:
    bool currentLocked(const Ticket& ticket) const {
        return ticket.owner_ == identity_ && ticket.version_ == requested_ &&
            ticket.version_ != published_.version;
    }
    // A retained ticket pins identity, not the owner or its published payload.
    // Reusing the owner's address cannot revive a ticket from its previous life.
    const std::shared_ptr<const Identity> identity_ = std::make_shared<const Identity>();
    mutable std::mutex mutex_;
    std::uint64_t requested_ = 0;
    Snapshot published_;
};
} // namespace engine
