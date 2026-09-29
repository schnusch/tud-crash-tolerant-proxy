local function slice(tbl, first, last)
  local result = {}
  for i = first or 1, last or #tbl do
    result[#result + 1] = tbl[i]
  end
  return result
end

function Div(elem)
  if not elem.classes:includes("figure") then
    return nil
  end

  local contents = elem.content
  local caption = nil
  if not elem.classes:includes("nocaption") then
    contents = slice(elem.content, 1, #elem.content - 1)
    caption = pandoc.Caption(elem.content[#elem.content].content)
  end

  return pandoc.Figure(contents, caption, elem.attr)
end
