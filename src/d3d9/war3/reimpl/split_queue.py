import os

header_file = r'e:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk\src\d3d9\war3\reimpl\war3_render_queue.h'
cpp_file = r'e:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk\src\d3d9\war3\reimpl\war3_render_queue.cpp'

with open(header_file, 'r', encoding='utf-8') as f:
    lines = f.readlines()

def find_block(start_line):
    brace_count = 0
    start_found = False
    for i in range(start_line, len(lines)):
        for char in lines[i]:
            if char == '{':
                start_found = True
                brace_count += 1
            elif char == '}':
                brace_count -= 1
                if start_found and brace_count == 0:
                    return i
    return -1

methods_to_extract = [
    ' BatchComparator(',
    ' ItemLess_Asm(',
    ' InnerSort(',
    ' FlushSortedItems_StdSort(',
    ' FlushTransparent_StdSort(',
    ' RenderBatch_Submit_Reimpl(',
    ' WorldObjects_RenderGroup_Reimpl('
]

cpp_content = ['#include "war3_render_queue.h"\n',
               '#include "../render/war3_render_state.h"\n',
               '#include "../tools/war3_perf_monitor.h"\n',
               '\nnamespace dxvk {\nnamespace war3 {\nnamespace reimpl {\n\n']

opt_perf_dtor = '    ~OptimizationPerfAcc() {'

i = 0
new_header_lines = []
while i < len(lines):
    line = lines[i]
    matched = False
    
    # Check if this line starts an extracted method
    for method in methods_to_extract:
        if method in line:
            # We found the declaration/definition line
            sig_start = i
            start_idx = sig_start
            while start_idx < len(lines) and '{' not in lines[start_idx]:
                start_idx += 1
            
            end_idx = find_block(start_idx)
            if end_idx != -1:
                matched = True
                # Extract to cpp
                sig_lines = lines[sig_start:start_idx+1]
                
                # Transform the signature for CPP
                for s_i, s in enumerate(sig_lines):
                    s = s.replace('static ', '')
                    s = s.replace(' BatchComparator(', ' RenderQueue::BatchComparator(')
                    s = s.replace(' ItemLess_Asm(', ' RenderQueue::ItemLess_Asm(')
                    s = s.replace(' InnerSort(', ' RenderQueue::InnerSort(')
                    s = s.replace(' FlushSortedItems_StdSort(', ' RenderQueue::FlushSortedItems_StdSort(')
                    s = s.replace(' FlushTransparent_StdSort(', ' RenderQueue::FlushTransparent_StdSort(')
                    s = s.replace(' RenderBatch_Submit_Reimpl(', ' RenderQueue::RenderBatch_Submit_Reimpl(')
                    s = s.replace(' WorldObjects_RenderGroup_Reimpl(', ' RenderQueue::WorldObjects_RenderGroup_Reimpl(')
                    cpp_content.append(s)
                
                # Add body
                cpp_content.extend(lines[start_idx+1:end_idx+1])
                cpp_content.append('\n')
                
                # Create declaration in header
                decl_str = ''.join(lines[sig_start:start_idx+1])
                decl_str = decl_str.split('{')[0].strip() + ';\n'
                new_header_lines.append(decl_str)
                
                i = end_idx
                break
    
    if not matched and opt_perf_dtor in line:
        sig_start = i
        start_idx = i
        end_idx = find_block(start_idx)
        if end_idx != -1:
            matched = True
            cpp_content.append('RenderQueue::RenderBatch_Submit_Reimpl::OptimizationPerfAcc::~OptimizationPerfAcc() {\n')
            cpp_content.extend(lines[start_idx+1:end_idx+1])
            cpp_content.append('\n')
            new_header_lines.append('    ~OptimizationPerfAcc();\n')
            i = end_idx

    if not matched:
        new_header_lines.append(line)
    
    i += 1

cpp_content.append('} // namespace reimpl\n} // namespace war3\n} // namespace dxvk\n')

with open(cpp_file, 'w', encoding='utf-8') as f:
    f.writelines(cpp_content)

with open(header_file, 'w', encoding='utf-8') as f:
    f.writelines(new_header_lines)

print('Split successful!')
