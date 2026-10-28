// coop/config/config_registry_rows_dev_vulkan.inc -- the [dev] rows of the Vulkan overlay arc: the red control of
// its proof and the re-initialisation drill. Included by config_registry_rows_dev.inc after its last row, as
// _dev_nb.inc is, so each row keeps its place in every expansion of the list. The entry forms are
// config_registry_rows.inc's.

CFG_ENUM(vk_overlay_red, "vk_overlay_red", "dev", "off", "off|selftest|present", "VOTVCOOP_VK_OVERLAY_RED",
    "Red control of the Vulkan overlay's proof (run the game with -vulkan): selftest skips the draw of the "
    "bring-up's offscreen frame, so the selftest line reads FAIL on the pixel and the overlay stays down; present "
    "makes the swapped present use the engine's own semaphore and submit nothing, so the frames line reads FAIL "
    "with submitted=0 while a surface is open.")
CFG_ENUM(vk_overlay_drill, "vk_overlay_drill", "dev", "off", "off|reinit", "VOTVCOOP_VK_OVERLAY_DRILL",
    "Drill (run the game with -vulkan): reinit runs the overlay's re-initialisation once on the live session, "
    "after the first frames verdict, then judges 60 more frames drawn on the rebuilt backend. A swapchain format "
    "or image-count change, which triggers the same code, cannot be forced from outside.")
