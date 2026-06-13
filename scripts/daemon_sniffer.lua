-- scripts/daemon_sniffer.lua
-- Native Wireshark Lua Dissector for the C++ StreamerDaemon

local filter = 'http2.header.name == ":path" or http.request.uri or http3.header.header.name == ":path"'
local tap = Listener.new("frame", filter)

-- Field extractors
local f_h2_name = Field.new("http2.header.name")
local f_h2_val  = Field.new("http2.header.value")
local f_h3_name = Field.new("http3.header.header.name")
local f_h3_val  = Field.new("http3.headers.header.value")
local f_h1_uri  = Field.new("http.request.uri")
local f_h1_host = Field.new("http.host")

-- Universal Media Keywords
local keywords = {
    "videoplayback", ".mp4", ".mkv", ".m3u8", ".webm", 
    "temp_url_", "/api/file/", "/get_file/", "/download/", "/video/", "/file/", "/stream/"
}

function tap.packet(pinfo, tvb, tapinfo)
    local authority = ""
    local path = ""
    local headers = {}

    local h2_names = { f_h2_name() }
    local h2_vals  = { f_h2_val() }
    local h3_names = { f_h3_name() }
    local h3_vals  = { f_h3_val() }

    -- Route A: HTTP/2
    if #h2_names > 0 and #h2_names == #h2_vals then
        for i, name_info in ipairs(h2_names) do
            local key = tostring(name_info.value)
            local val = tostring(h2_vals[i].value)
            if key == ":authority" then authority = val
            elseif key == ":path" then path = val
            elseif string.sub(key, 1, 1) ~= ":" and key ~= "accept-encoding" then
                headers[key] = val
            end
        end
    -- Route B: HTTP/3
    elseif #h3_names > 0 and #h3_names == #h3_vals then
        for i, name_info in ipairs(h3_names) do
            local key = tostring(name_info.value)
            local val = tostring(h3_vals[i].value)
            if key == ":authority" then authority = val
            elseif key == ":path" then path = val
            elseif string.sub(key, 1, 1) ~= ":" and key ~= "accept-encoding" then
                headers[key] = val
            end
        end
    -- Route C: HTTP/1.1
    else
        local h1_uri = f_h1_uri()
        local h1_host = f_h1_host()
        if h1_uri and h1_host then
            path = tostring(h1_uri.value)
            authority = tostring(h1_host.value)
        end
    end

    if authority == "" or path == "" then return end

    -- Fast Media Sanity Check in Native Lua
    local is_media = false
    for _, kw in ipairs(keywords) do
        if string.find(path, kw, 1, true) then
            is_media = true
            break
        end
    end

    if not is_media then return end

    -- Format the payload for the C++ Daemon: URL|key=val^key=val
    local full_url = "https://" .. authority .. path
    local header_str = ""
    
    for k, v in pairs(headers) do
        header_str = header_str .. k .. "=" .. v .. "^"
    end

    print("[DAEMON_HOOK] " .. full_url .. "|" .. header_str)
end
