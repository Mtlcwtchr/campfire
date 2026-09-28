#include "engine/render/impostor_cache.hpp"
#include <chrono>
#include <stdexcept>

namespace engine::render {
ImpostorCache::ImpostorCache(std::size_t capacity,std::size_t byteBudget,ImpostorBakeOptions options,Baker baker)
    : slots_(capacity),pending_(capacity),options_(options),baker_(std::move(baker)),budget_(byteBudget) {
    if (!capacity || capacity>128 || !budget_ || !baker_) throw std::invalid_argument("invalid impostor cache budget");
}
ImpostorCache::~ImpostorCache() { if (cancel_) *cancel_=true; if (job_.valid()) job_.wait(); }
void ImpostorCache::frame(double seconds,std::uint64_t completedSubmission) {
    if (!std::isfinite(seconds) || seconds<now_) throw std::invalid_argument("invalid impostor cache clock");
    now_=seconds;completed_=std::max(completed_,completedSubmission);
}
void ImpostorCache::discard(std::size_t i) {
    auto& slot=slots_[i];
    if (slot.atlas) bytes_-=slot.atlas->bytes();
    slot.atlas.reset();pending_[i].clear();slot.uploadedViews=0;slot.ticket=++clock_;slot.state=State::Empty;
    // lastSubmission must survive invalidation: old GPU work still reads that slot.
}
int ImpostorCache::claim(ImpostorKey key,std::uint64_t revision) {
    int chosen=-1;
    for (std::size_t i=0;i<slots_.size();++i) if (slots_[i].state!=State::Empty && slots_[i].key==key) {
        if (slots_[i].revision==revision) {slots_[i].requested=now_;return -2-int(i);}
        discard(i);chosen=int(i);break;
    }
    if (chosen<0) for (std::size_t i=0;i<slots_.size();++i) {
        const auto& slot=slots_[i];
        if (slot.state==State::Empty && slot.lastSubmission<=completed_) {chosen=int(i);break;}
        if (slot.state!=State::Baking && slot.requested+0.75<now_ && slot.lastSubmission<=completed_ &&
            (chosen<0 || slot.requested<slots_[std::size_t(chosen)].requested)) chosen=int(i);
    }
    if (chosen<0) return -1;
    const auto i=std::size_t(chosen);discard(i);
    auto& slot=slots_[i];slot.key=key;slot.revision=revision;slot.requested=now_;
    return chosen;
}
int ImpostorCache::request(ImpostorKey key,std::uint64_t revision,std::span<const ImpostorPlacement> members) {
    if (members.empty() || members.size()>options_.maxMembers) return -1;
    const int chosen=claim(key,revision);
    if (chosen<=-2) return -2-chosen;
    if (chosen<0) return -1;
    slots_[std::size_t(chosen)].state=State::Queued;
    pending_[std::size_t(chosen)].assign(members.begin(),members.end());
    return chosen;
}
int ImpostorCache::adopt(ImpostorKey key,std::uint64_t revision,std::shared_ptr<const ImpostorAtlas> atlas) {
    if (!atlas || !atlas->valid()) return -1;
    // A current slot is only touched; checking the budget first would evict
    // nothing and refuse a slot that is already paid for.
    for (std::size_t i=0;i<slots_.size();++i)
        if (slots_[i].state!=State::Empty && slots_[i].key==key && slots_[i].revision==revision) {
            slots_[i].requested=now_;return int(i);
        }
    const int chosen=claim(key,revision);
    if (chosen<=-2) return -2-chosen;
    if (chosen<0) return -1;
    auto& slot=slots_[std::size_t(chosen)];
    if (atlas->bytes()>budget_-bytes_) {slot.state=State::Empty;return -1;}
    bytes_+=atlas->bytes();slot.atlas=std::move(atlas);slot.state=State::Ready;++completedBakes_;
    return chosen;
}
void ImpostorCache::invalidate(ImpostorKey key) {
    for (std::size_t i=0;i<slots_.size();++i) if (slots_[i].state!=State::Empty && slots_[i].key==key) discard(i);
}
void ImpostorCache::clear() {
    if (cancel_) *cancel_=true;
    for (std::size_t i=0;i<slots_.size();++i) discard(i);
}
void ImpostorCache::poll() {
    if (job_.valid() && job_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        const auto result=job_.get();auto& slot=slots_[result.slot];
        if (slot.ticket!=result.ticket || slot.state!=State::Baking) ++staleBakes_;
        else if (!result.atlas || !result.atlas->valid() || result.atlas->bytes()>budget_-bytes_) slot.state=State::Failed;
        else {slot.atlas=result.atlas;bytes_+=result.atlas->bytes();slot.state=State::Ready;++completedBakes_;}
    }
    if (job_.valid()) return;
    for (std::size_t i=0;i<slots_.size();++i) if (slots_[i].state==State::Queued) {
        if (slots_[i].requested+0.75<now_) {discard(i);continue;}
        auto members=std::move(pending_[i]);slots_[i].state=State::Baking;
        const auto ticket=slots_[i].ticket;cancel_=std::make_shared<std::atomic_bool>(false);
        job_=std::async(std::launch::async,[i,ticket,members=std::move(members),options=options_,baker=baker_,cancel=cancel_] {
            std::shared_ptr<const ImpostorAtlas> atlas;
            try {atlas=baker(members,options,cancel.get());} catch (...) {atlas.reset();}
            return Result{i,ticket,std::move(atlas)};
        });
        break;
    }
}
int ImpostorCache::uploadCandidate() const {
    for (std::size_t i=0;i<slots_.size();++i) {
        const auto& slot=slots_[i];
        if ((slot.state==State::Ready || slot.state==State::Uploading) && slot.lastSubmission<=completed_) return int(i);
    }
    return -1;
}
void ImpostorCache::uploaded(std::size_t i,unsigned views,bool success,std::uint64_t uploadSerial) {
    auto& slot=slots_.at(i);
    if ((slot.state!=State::Ready && slot.state!=State::Uploading) || slot.lastSubmission>completed_ ||
        views==0 || views>21-slot.uploadedViews) throw std::logic_error("invalid impostor publication");
    slot.lastSubmission=std::max(slot.lastSubmission,uploadSerial);
    if (!success) {discard(i);slot.state=State::Failed;return;}
    slot.uploadedViews+=views;slot.state=slot.uploadedViews==21?State::Resident:State::Uploading;
}
const ImpostorCache::Slot* ImpostorCache::resident(ImpostorKey key,std::uint64_t revision) const {
    for (const auto& slot:slots_) if (slot.state==State::Resident && slot.key==key && slot.revision==revision) return &slot;
    return nullptr;
}
void ImpostorCache::protect(std::size_t i,std::uint64_t submission) {
    auto& slot=slots_.at(i);if (slot.state!=State::Resident) throw std::logic_error("protecting an unpublished impostor");
    slot.lastSubmission=std::max(slot.lastSubmission,submission);
}
} // namespace engine::render
