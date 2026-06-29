function Code(elem)
    if not elem.classes:includes("manpage") then
        return nil
    end

    local name, section = elem.text:match("^([%w_]+)%((%d+)%)$")
    if not name then
        return nil
    end

    local url = string.format(
        "https://man7.org/linux/man-pages/man%s/%s.%s.html",
        section, name, section
    )

    return pandoc.Link(elem, url)
end
