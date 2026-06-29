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
    { elem.content[1] },
    pandoc.Caption(slice(elem.content, 2)),
    elem.attr
  )
end
