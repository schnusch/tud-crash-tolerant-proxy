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

  return pandoc.Figure(
    slice(elem.content, 1, #elem.content - 1),
    pandoc.Caption(elem.content[#elem.content].content),
    elem.attr
  )
end
