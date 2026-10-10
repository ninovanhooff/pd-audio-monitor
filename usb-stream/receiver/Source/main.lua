-- Lua runs the frame so the game keeps the Lua runtime, which `eval` needs. Both functions are
-- defined in C (src/main.c).
function playdate.update()
    pdstream_update()
end
