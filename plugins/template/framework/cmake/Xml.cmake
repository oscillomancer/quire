# Safe in XML text and double-quoted attributes. Ampersands go first so
# inserted entities are not escaped again.
function(supple_xml_escape out text)
    string(REPLACE "&" "&amp;" text "${text}")
    string(REPLACE "<" "&lt;" text "${text}")
    string(REPLACE ">" "&gt;" text "${text}")
    string(REPLACE "\"" "&quot;" text "${text}")
    string(REPLACE "'" "&apos;" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()
