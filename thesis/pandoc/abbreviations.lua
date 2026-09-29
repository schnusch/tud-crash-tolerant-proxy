function Str(elem)
  local replace = {
    ["^([Ee])%.g%."] = "%1.\u{202F}g.",
    ["([%s%p][Ee])%.g%."] = "%1.\u{202F}g.",
    ["^([I])%.e%."] = "%1.\u{202F}e.",
  }
  for from, to in pairs(replace) do
    elem.text = elem.text:gsub(from, to)
  end
  return elem
end
