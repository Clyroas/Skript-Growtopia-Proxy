#pragma once
#include "../extension.hpp"

namespace extension::player_state_tracker {

class IPlayerStateTrackerExtension : public IExtension {
public:
    PROVIDE_EXT_UID(0xDEADBEEF7890)
    
    virtual ~IPlayerStateTrackerExtension() = default;
    
    // X23: was an empty free() - the only one of 15 extensions that leaked itself.
    void free() override { delete this; }
};

} 
