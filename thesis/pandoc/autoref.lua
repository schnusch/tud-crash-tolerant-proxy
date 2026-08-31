function RawInline(elem)
    if FORMAT == "latex" then
        return nil
    end

    local ref = elem.text:match("^\\autoref{(.*)}$")
    if not ref then
        return nil
    end

    ref = "#" .. ref
    return pandoc.Link(
        {
            pandoc.Str("ref"),
            pandoc.Space(),
            pandoc.Code(ref),
        },
        ref
    )
end
