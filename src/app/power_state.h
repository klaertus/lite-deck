#pragma once
namespace ld {

// Lock and system sleep are independent: the device wakes only when neither is active.
// Each call returns true when the caller must now sleep (on_lock/on_suspend) or wake (on_unlock/on_resume).
class PowerState {
public:
  bool on_lock() { return enter(locked_); }
  bool on_suspend() { return enter(sleeping_); }
  bool on_unlock() { return leave(locked_); }
  bool on_resume() { return leave(sleeping_); }

private:
  bool enter(bool& flag) {
    const bool was_awake = !locked_ && !sleeping_;
    flag = true;
    return was_awake;
  }
  bool leave(bool& flag) {
    const bool was_down = locked_ || sleeping_;
    flag = false;
    return was_down && !locked_ && !sleeping_;
  }
  bool locked_ = false, sleeping_ = false;
};

}  // namespace ld
