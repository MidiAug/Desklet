#pragma once
#include <string>
namespace tongzhuo {
// The only production path allowed to request a switch is a user command.
// Read-only refresh, notifications and shutdown never receive this capability.
struct SwitchGate {
    unsigned switchCalls=0;
    template<class Id,class Switch>
    bool userSelect(const Id& current,const Id& target,Switch&& invoke) {
        if(current==target)return true;
        ++switchCalls;return invoke(target);
    }
};
}
