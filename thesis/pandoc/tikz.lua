function Image(elem)
  if not elem.src:match("%.tex$") then
    return nil
  elseif FORMAT == "latex" then
    return pandoc.RawInline("latex", "\\input{" .. elem.src .. "}")
  else
    elem.src = elem.src:gsub("%.tex$", ".svg")
    return elem
  end
end
