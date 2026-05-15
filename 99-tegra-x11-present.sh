# /etc/profile.d/99-tegra-x11-present.sh
#
# NVIDIA driver env vars required by vk_layer_tegra_x11_present to keep the
# Vulkan→GL relay worker thread off the CPU during vsync waits.
#
# On Tegra L4T r32.x, libnvidia-glcore caches __GL_YIELD at libGL load time.
# Setting it from inside the Vulkan layer (even via a .so constructor) runs
# too late — the value is already cached by the time we get a chance. The
# only reliable way to apply it is from the shell environment that launches
# the GLX-using process, which is what this profile.d script does.
#
# USLEEP makes glXSwapBuffers's vsync wait call usleep(1) between vblank
# checks instead of sched_yield(), so the kernel can actually idle the
# thread. Without this, the worker thread shows ~100% CPU in top even
# though the real work is trivial.
#
# See: https://github.com/KDE/kwin/commit/3ce5af5c21fd80e3da231b50c39c3ae357e9f15c
# for the upstream KDE/KWin discussion of the same issue.

export __GL_YIELD=USLEEP
