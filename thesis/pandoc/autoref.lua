function RawInline(elem)
    if FORMAT == "latex" then
        return nil
    end

    local type = nil
    local ref = nil
    for _, pattern in ipairs({
        "^(\\[Cc]ref){(.*)}$",
        "^(\\[Ff]ullref){(.*)}$",
        "^(\\nameref){(.*)}$",
    }) do
        type, ref = elem.text:match(pattern)
        if type ~= nil then
            break
        end
    end
    if type == nil or ref == nil then
        return nil
    end

    return pandoc.Link(
        {
            pandoc.Code(type .. "{" .. ref .. "}", { class = "latetx" }),
        },
        "#" .. ref
    )
end
