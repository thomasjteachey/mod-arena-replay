# Arena replay ghost GUID design

## Purpose
Arena replays must allow a player to watch **their own match** without replay packets being addressed to their live character. To guarantee this, the replay system **remaps every participant GUID** (including the viewer if they participated) to a **unique ghost GUID** before packets are sent. This ensures the client receives packets that describe **ghost copies** of all participants, not the real player objects.

## Key design points
- **All participant GUIDs are remapped** to ghost GUIDs, not just the viewer. This avoids any original GUIDs appearing in replay packets.
- **Replay packets are rewritten** to replace original GUIDs with ghost GUIDs. Packets are kept after rewriting to avoid false-positive GUID matches causing dropped data.
- This design is critical to avoid crashes or undefined behavior when a player watches their own replay, because the client must never see packets targeting their real GUID.

## Where to look in code
- `src/ArenaReplay.cpp` implements GUID remapping and packet rewriting for replays.
