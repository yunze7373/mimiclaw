-- example_gpio.lua — Example hardware skill for MimiClaw
--
-- Demonstrates the Lua skill format:
--   SKILL table: metadata (name, version, description)
--   TOOLS table: agent-callable functions with JSON schemas
--   init():      called on load (optional)
--
-- Place this file in /spiffs/skills/ to auto-load on boot.

SKILL = {
    name    = "example_gpio",
    version = "1.0",
    desc    = "Example skill: GPIO read/write and system info",
}

function init(config)
    hw.log("example_gpio skill loaded!")
    return true
end

-- Read a GPIO pin level
function read_pin(params)
    local pin = params.pin or 0
    hw.gpio_set_mode(pin, "input_pullup")
    local val = hw.gpio_read(pin)
    return {
        pin   = pin,
        level = val,
    }
end

-- Write a GPIO pin
function write_pin(params)
    local pin = params.pin or 0
    local val = params.value or 0
    hw.gpio_set_mode(pin, "output")
    hw.gpio_write(pin, val)
    return {
        pin   = pin,
        value = val,
        status = "ok",
    }
end

-- Get free heap memory
function heap_info(params)
    local free = hw.free_heap()
    return {
        free_bytes = free,
    }
end

TOOLS = {
    {
        name    = "skill_gpio_read",
        desc    = "Read the level of a GPIO pin (0 or 1). Uses internal pull-up.",
        schema  = '{"type":"object","properties":{"pin":{"type":"integer","description":"GPIO pin number"}},"required":["pin"]}',
        handler = read_pin,
    },
    {
        name    = "skill_gpio_write",
        desc    = "Set a GPIO pin to high (1) or low (0).",
        schema  = '{"type":"object","properties":{"pin":{"type":"integer","description":"GPIO pin number"},"value":{"type":"integer","description":"0 or 1"}},"required":["pin","value"]}',
        handler = write_pin,
    },
    {
        name    = "skill_heap_info",
        desc    = "Get the current free heap memory in bytes.",
        schema  = '{"type":"object","properties":{}}',
        handler = heap_info,
    },
}
