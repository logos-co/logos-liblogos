#ifndef LOGOS_INSTANCE_ID_H
#define LOGOS_INSTANCE_ID_H

namespace logos {

// Sets LOGOS_INSTANCE_ID unless it is set: it is in every socket name, and the
// processes this one spawns inherit it.
void ensureInstanceId();

} // namespace logos

#endif
