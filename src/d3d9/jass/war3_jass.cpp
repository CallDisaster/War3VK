#include "war3_jass_types.h"
#include "war3_game.h"
#include "war3_hook.h"
#include "war3_jass_native.h"
#include "war3_jass_convert.h"
#include <sstream>
#include <iomanip>
#include <cstring>

// 简易日志
#define LOG_ERROR(msg) std::wcerr << L"[JASS_ERROR] " << msg << std::endl
// 简易格式化
#define XFMT(s) s


namespace jass {
	// 函数指针
	static uintptr_t getDataNode;
	static uintptr_t getInstance;
	static uintptr_t getJassVM;
	static uintptr_t initJassVM;
	static uintptr_t getJassFuncStruct;
	static uintptr_t getJassVariable;
	static uintptr_t executeFunc;
	static uintptr_t executeCode;
	static uintptr_t codeToIndex;
	static uintptr_t getInstanceGenerator;
	static uintptr_t generateInstance;
	static uintptr_t generateAgent;
	static uintptr_t getAgentHashKey;
	static uintptr_t getAgentDataNode;
	static uintptr_t getUIDefData;
	static uintptr_t getCObject;
	static uintptr_t getCAgent;
	static uintptr_t setCAgent;
	static uintptr_t isCAgentValid;
	static uintptr_t checkInherit;
	static uintptr_t unrefObj;
	static uintptr_t orderIdToString;
	static uintptr_t getCurEventId;
	static uintptr_t getEventDataCall1;
	static uintptr_t getEventDataCall2;
	static uintptr_t opcodeMOVRL;
	static uintptr_t getJassArrayValue;
	static uintptr_t setJassArrayValue;
	static uintptr_t processStringOldValue;
	static uintptr_t addSymbol;
	static uintptr_t copyThread;

	static uintptr_t jassParser;
	static uintptr_t pushReturn;

	struct AgentGenerateData {
		uint32_t field[0xB];
	};

	// 回调线程缓存
	static std::vector<JassVM*> callback_threads;


	// C++ 包装为脚本代码
	namespace cpp_closure {
		// 闭包信息
		using closure_table_t = std::unordered_map<uint32_t, const closure_t*>;

		// 空的闭包
		static const closure_t _NULL_CLOSURE = closure_t(0, "", nullptr, FALSE);

		// 闭包表
		static closure_table_t closure_table;

		// JASS 执行回调
		static uintptr_t jump_in = 0;

		// JASS 执行回调已 Hook
		static bool initialized = false;

		static closure_stack_t closure_stack;

		// JASS 执行回调
		static int32_t __fastcall handler(uintptr_t cjassfunc, uint32_t, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
	}

	// 带参闭包
	namespace code_closure {

		// 空的闭包
		static const closure_info_t _NULL_CLOSURE = closure_info_t();

		// 构造的指令表 [hash]
		static std::unordered_map<uint32_t, ClosureInfo> closure_table;

		// 当前寄存器编号
		static byte_t register_index = 0;

		// 分配寄存器编号 1~255轮换
		static byte_t register_allocate();
	}
    
    // 字符串表缓存
    std::unordered_map<const char**, std::pair<std::string, const char*>> str_table;

    // 十六进制转换
    std::string to_hex_string(uintptr_t val) {
        std::stringstream ss;
        ss << std::hex << val;
        return ss.str();
    }
};

using namespace jass;

//==================================================================================
//
// 接口函数
//
//==================================================================================

// 获取实例
void* jass::get_instance(uint32_t id) {
	return call_fast<void*>(getInstance, id);
}

// 获取数据节点
void* jass::get_data_node() {
	return call_fast<void*>(getDataNode, game_war3);
}

// 获取jass虚拟机
JassVM* jass::get_jass_vm(uint32_t index) {
	return call_fast<JassVM*>(getJassVM, index);
}

// 获取主线程
JassVM* jass::get_main_thread() {
	return get_jass_vm(1);
}

// 获取当前线程
JassVM* jass::get_cur_thread() {
	uintptr_t ptr = (uintptr_t)get_instance(5);
    if (!ptr) return nullptr;
	uint32_t index = *(uint32_t*)(ptr + 0x14);

	return index ? *(JassVM**)(*(uintptr_t*)(ptr + 0xC) + 4 * index - 4) : nullptr;
}

// 获取回调线程
JassVM* __fastcall jass::get_callback_thread() {
	JassVM* res = nullptr;

	if (callback_threads.size())
	{
		res = callback_threads.back();
		callback_threads.pop_back();
	}
	else
	{
		// 复制一个新的线程
		res = get_jass_vm(call_fast<uint32_t>(copyThread, game_war3->cur_thread));
	}

	return res;
}

// 执行code
int32_t	__fastcall jass::exec_code(const OPCode* code) {
	auto jvm = get_callback_thread();
	int32_t res = 0;
	if ((intptr_t)jvm > 0)
		res = jvm->exec_code(code);
	callback_threads.push_back(jvm);
	return res;
}

// 执行code
int32_t	__fastcall jass::exec_code(const string& code) {
	auto	jvm = get_callback_thread();
	int32_t res = 0;
	if ((intptr_t)jvm > 0)
		res = jvm->exec_code(code);
	callback_threads.push_back(jvm);
	return res;
}

// 执行code
int32_t	__fastcall jass::exec_code(jCode code) {
	auto	jvm = get_callback_thread();
	int32_t res = 0;
	if ((intptr_t)jvm > 0)
		res = jvm->exec_code(code);
	callback_threads.push_back(jvm);
	return res;
}

// 检查继承关系
bool jass::check_inherit(uint32_t child_class, uint32_t parent_class) {
	return (child_class && parent_class) ? call_fast<uint32_t>(checkInherit, child_class, parent_class) : false;
}

bool jass::check_inherit(void* obj, uint32_t parent_class) {
	uint32_t type = get_obj_type(obj);
	return type ? check_inherit(type, parent_class) : false;
}

// 创建agent
void* __fastcall jass::create_agent(uint32_t id, uint32_t param) {
	uintptr_t ptr = call_fast<uintptr_t>(getInstanceGenerator, id);
	if (!ptr)
		return nullptr;

	AgentGenerateData data = AgentGenerateData();
	call_fast<void>(generateInstance, &data, id, *(void**)(ptr + 0x70));
	data.field[9] = 0xFFFFFFFF;

	ptr = call_fast<uintptr_t>(generateAgent, &data, param + 1, 1);
	if (!ptr)
		return nullptr;
	ptr = *(uintptr_t*)(ptr + 0x54);
	if (ptr && check_inherit((void*)ptr, id))
		return (void*)ptr;

	return nullptr;
}

uint32_t jass::get_agent_hash_key(uint32_t id) {
	return call_this<uint32_t>(getAgentHashKey, &id);
}

void* jass::get_agent_data_node(uintptr_t dataNode, uint32_t aid) {
	uint32_t hash_key = get_agent_hash_key(aid);
	return hash_key ? call_this<void*>(getAgentDataNode, dataNode, hash_key, &aid) : nullptr;
}

void* jass::get_ui_data_node(uint32_t id) {
	return call_fast<void*>(getUIDefData, id);
}

void* jass::get_object(uint32_t hash1, uint32_t hash2) {
	return call_fast<void*>(getCObject, hash1, hash2);
}

void* jass::get_object(HashGroup const& hash) {
	return call_fast<void*>(getCObject, hash.hash_0x0, hash.hash_0x4);
}

void* jass::get_agent(HashGroup const& hash) {
	return call_fast<void*>(getCAgent, &hash);
}

void* jass::get_agent(uint32_t hash1, uint32_t hash2) {
	return get_agent(HashGroup(hash1, hash2));
}

void jass::set_agent(HashGroup& hash, void* obj) {
	if (hash.is_exist())
		call_this<void>(setCAgent, &hash, obj);
}

bool jass::is_agent_valid(void* obj) {
	return obj ? call_fast<uint32_t>(isCAgentValid, obj) : false;
}

// 引用handle
bool jass::ref_handle(jHandle h) {
	return get_main_thread()->handle_ref(h, true);
}

// 解引用handle
bool jass::unref_handle(jHandle h) {
	return get_main_thread()->handle_ref(h, false);
}

// 解引用
void* jass::unref_obj(void* obj) {
	return call_fast<void*>(unrefObj, obj);
}

// 获取对象类型
uint32_t jass::get_obj_type(void* obj) {
    // 取虚表偏移的简化实现
    // *(uintptr_t*)(*(uintptr_t*)obj + offset)
    if (!obj) return 0;
    uintptr_t vtable = *(uintptr_t*)obj;
    if (!vtable) return 0;
    uintptr_t func_addr = *(uintptr_t*)(vtable + 0x1C);
	return func_addr
		? call_fast<uint32_t>(func_addr, obj)
		: 0;
}

// 获取handle类型
uint32_t jass::get_handle_type(jHandle handle) {
	void* obj = convert::to_object<void*>(handle);
	return obj ? get_obj_type(obj) : 0;
}

// 对象是widget
bool jass::is_obj_widget(void* obj) {
	uint32_t type = get_obj_type(obj);
	// '+w3u' 等为多字符常量
    // 'item' 对应 0x6974656D
	return (type == 0x2B773375) || (type == 0x2B773364) || (type == 0x6974656D);
}

// handle是有效的
bool jass::is_handle_valid(jHandle handle) {
	return (handle > 0x100000);
}

char* jass::order_to_string(uint32_t order) {
	char* res = call_this<char*>(orderIdToString, game_war3, order);
	return (res && *res) ? res : nullptr;
}

uint32_t jass::get_event_id() {
	return call_std<uint32_t>(getCurEventId);
}

uintptr_t jass::get_event_data() {
	return call_std<uint32_t>(getEventDataCall1)
		? call_std<uintptr_t>(getEventDataCall2)
		: 0;
}

bool __fastcall jass::replace_string(const char** addr, const char* str) {
	if (!addr)
		return false;

	if (!str || !*str)
		str = "";

	auto it = str_table.find(addr);
	if (it != str_table.end())
		it->second.first = str;
	else
		str_table[addr] = { str, *addr };

	*addr = str_table[addr].first.c_str();
	return true;
}

bool __fastcall jass::reset_string(const char** addr) {
	if (!addr)
		return false;
	auto it = str_table.find(addr);
	if (it == str_table.end())
		return false;
	*addr = it->second.second;
	str_table.erase(it);
	return true;
}

jass::handle_list_t jass::get_handle_list() {
	handle_list_t list;
	if (!game_state)
		return list;
	auto& table = game_state->handle_table;
	for (uint32_t i = 0; i < table.size; ++i) {
		auto& node = table.handle_array[i];
		if (node.object)
			list.push_back(handle_node_t{ node.ref_count, node.object });
	}
	return list;
}

uint32_t jass::get_max_handle_count() {
	return game_state ? game_state->handle_table.size : 0;
}

//==================================================================================
// C++ 闭包
//==================================================================================

int32_t __fastcall cpp_closure::handler(uintptr_t cjassfunc, uint32_t, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
	jCode	code	= *(jCode*)(cjassfunc + 0x24);
	auto	jvm		= get_jass_vm(game_war3->cur_thread);

	if (jvm) {
		uint32_t size = *(uint32_t*)(jvm->get_vm()->code_table + 0x4);
		if (code >= size) {
			auto& closure = get(code);
			if (closure)
				return closure();
		}
	}

	return call_this<int32_t>(jump_in, cjassfunc, a1, a2, a3, a4, a5);
}

const cpp_closure::closure_t& __fastcall cpp_closure::make(const string& name, const callback_t& callback, bool cause_desync) {
	if (!callback)
		return _NULL_CLOSURE;

	if (!initialized) {
		if (!jump_in) {
            // 跳转入口未就绪
			return _NULL_CLOSURE;
		}
		initialized = true;
		hook::detours::create(&jump_in, (void*)handler);
	}

	auto hash = closure_t::HASH(name);

	auto it = closure_table.find(hash);
	if (it != closure_table.end())
		return *it->second;

	auto& closure = closure_table[hash];
	closure = new closure_t(hash, name, callback, cause_desync);
	
	return *closure;
}

const cpp_closure::closure_t& cpp_closure::get(jCode code) {
	auto it = closure_table.find(code);
	return it != closure_table.end()
		? *it->second
		: _NULL_CLOSURE;
}

const cpp_closure::closure_t& cpp_closure::get(const string& name) {
	return get(closure_t::HASH(name));
}

const cpp_closure::closure_stack_t& cpp_closure::get_stack() {
	return closure_stack;
}

const cpp_closure::closure_t& cpp_closure::get_frame() {
	return closure_stack.empty() ? _NULL_CLOSURE : *closure_stack.back();
}


// 闭包信息实现
jCode cpp_closure::_Closure_Info::get_code() const {
	return this ? code : 0;
}
const string& cpp_closure::_Closure_Info::get_name() const {
    static string nullstring = "";
	return this ? name : nullstring;
}
BOOL cpp_closure::_Closure_Info::can_cause_desync() const {
	return this ? cause_desync : false;
}
uint32_t cpp_closure::_Closure_Info::operator()() const {
	if (!callback)
		return 0;
	closure_stack.push_back(this);
	uint32_t retval = callback();
	closure_stack.pop_back();
	return retval;
}
bool cpp_closure::_Closure_Info::valid() const {
    return this && this != &_NULL_CLOSURE;
}
cpp_closure::_Closure_Info::operator bool() const {
    return valid();
}
bool cpp_closure::_Closure_Info::operator==(const _Closure_Info& that) const {
	return code == that.code;
}
bool cpp_closure::_Closure_Info::operator!=(const _Closure_Info& that) const {
	return !((*this) == that);
}
uint32_t cpp_closure::_Closure_Info::HASH(const string& str) {
    // FNV-1a 哈希
    uint32_t hash = 2166136261u;
    for(char c : str) { hash ^= c; hash *= 16777619u; }
    return hash;
}

//==================================================================================
// 代码闭包
//==================================================================================

byte_t code_closure::register_allocate() {
	++register_index;
	if (register_index >= 0xFF)
		register_index = 1;
	return register_index;
}

const code_closure::closure_info_t& __fastcall code_closure::make(const string& name, const param_info_t& param_info, const string& prefix) {
	uint32_t hash = closure_info_t::hash(name);

	auto it = closure_table.find(hash);
	if (it != closure_table.end())
		return it->second;

	auto* jvm = jass::get_main_thread();
	if (!jvm)
		return _NULL_CLOSURE;

	auto* code = jvm->get_jass_func(name);
	if (!code)
		return _NULL_CLOSURE;

	auto* symbol_table = jvm->get_symbol_table();
	if (!symbol_table)
		return _NULL_CLOSURE;

	auto* symbol = symbol_table->find(name);
	if (!symbol)
		return _NULL_CLOSURE;

	auto* opcode = code->func_addr;
	if (!opcode)
		return _NULL_CLOSURE;

	auto* c_symbol = symbol_table->add_symbol(prefix + name);
	if (!c_symbol)
		return _NULL_CLOSURE;

	auto& info = closure_table[hash];
	info.name = name;
	info.param_info = param_info;

	auto& closure = info.closure;
	closure.clear();
	info.positions.clear();

	{
		// 函数开始
		closure.push_back(OPCode(
			Parser::OPCODE_FUNCTION, 0, 0, 0, c_symbol->index
		));

		// 压栈 (从左到右)
		for (auto& type : param_info.param_type) {
			byte_t rid = register_allocate();

			closure.push_back(OPCode(
				Parser::OPCODE_MOVRL, rid, type, 0, 0
			));
			info.positions.push_back(static_cast<uint32_t>(closure.size() - 1));

			closure.push_back(OPCode(
				Parser::OPCODE_PUSH, rid, 0, 0, 0
			));
		}

		// 调用
		closure.push_back(OPCode(
			Parser::OPCODE_CALLJASS, 0, 0, 0, symbol->index
		));

		// 出栈
		if (!param_info.param_type.empty()) {
			closure.push_back(OPCode(
				Parser::OPCODE_POPN, static_cast<byte_t>(param_info.param_type.size()), 0, 0, 0
			));
		}

		// 返回
		closure.push_back(OPCode(
			Parser::OPCODE_RETURN, 0, 0, 0, 0
		));

		// 函数结束
		closure.push_back(OPCode(
			Parser::OPCODE_ENDFUNCTION, 0, 0, 0, 0
		));
	}

	info.j_code = jvm->to_jCode(&closure[1]);
	return info;
}

const code_closure::closure_info_t& code_closure::get(jCode code) {
	for (auto& [_, info] : closure_table) {
		if (info.j_code == code)
			return info;
	}
	return _NULL_CLOSURE;
}

const code_closure::closure_info_t& code_closure::get(const string& name) {
	auto it = closure_table.find(closure_info_t::hash(name));
	return it != closure_table.end() ? it->second : _NULL_CLOSURE;
}

code_closure::ClosureInfo::operator bool() const {
	return this && (*this) != _NULL_CLOSURE;
}

bool code_closure::ClosureInfo::operator==(const ClosureInfo& that) const {
	return j_code == that.j_code;
}

bool code_closure::ClosureInfo::operator!=(const ClosureInfo& that) const {
	return !((*this) == that);
}

int32_t code_closure::ClosureInfo::operator()(const param_buffer_t& args) const {
	if (!(*this))
		return 0;

	const uint32_t param_count = static_cast<uint32_t>(param_info.param_type.size());
	for (uint32_t i = 0; i < param_count; ++i) {
		*const_cast<uint32_t*>(&closure[positions[i]].arg) = args[i];
	}

	return (*this)();
}

int32_t code_closure::ClosureInfo::operator()() const {
	auto* jvm = jass::get_main_thread();
	if (!jvm)
		return 0;
	return jvm->exec_code(&closure[1]);
}

uint32_t code_closure::ClosureInfo::hash(const string& str) {
	uint32_t hash = 2166136261u;
	for (char c : str) {
		hash ^= static_cast<uint8_t>(c);
		hash *= 16777619u;
	}
	return hash;
}


//==================================================================================
// JassVM
//==================================================================================

const CStringRep	JassVM::NULL_CSTRINGREP = CStringRep((char*)"");
const RCString		JassVM::NULL_RCSTRING	= RCString(JassVM::NULL_CSTRINGREP);

// CStringRep 构造函数实现
CStringRep::CStringRep(const char* _str) 
    : vf_table(nullptr), ref_count(1), string_hash(0), next_string(nullptr), str(_str) {}
CStringRep::CStringRep() 
    : vf_table(nullptr), ref_count(1), string_hash(0), next_string(nullptr), str("") {}
const char* CStringRep::get_str() const { return str; }

// RCString 构造函数实现
RCString::RCString(const CStringRep& rep) 
    : vf_table(nullptr), ref_count(1), string_rep(&rep) {}
RCString::RCString() 
    : vf_table(nullptr), ref_count(1), string_rep(nullptr) {}
const char* RCString::get_str() const { return string_rep ? string_rep->get_str() : nullptr; }

JassVMStruct* JassVM::get_vm() {
	return &jass_vm;
}

JassRegister* JassVM::get_register(uint32_t index) {
	if (index < 0x100) {
		return &(jass_vm.reg[index]);
	}
	return nullptr;
}

JassStackFrame* JassVM::get_stack_frame() {
	return (JassStackFrame*)(jass_vm.stack_struct);
}

JassSymbolTable* JassVM::get_symbol_table() {
	return this ? (JassSymbolTable*)jass_vm.symbol_table : nullptr;
}

ScriptDataTable* JassVM::get_global_table() {
	return jass_vm.global_table;
}

JassStringTable* JassVM::get_string_table() {
	return this ? jass_vm.string_table : nullptr;
}

uint32_t JassVM::get_string_count() {
	auto table = get_string_table();
	return table ? table->size : 0;
}

JassFuncStruct* JassVM::get_jass_func(const string& func_name) {
	return this && func_name.size()
		? call_this<JassFuncStruct*>(getJassFuncStruct, jass_vm.func_table, func_name.c_str())
		: nullptr;
}

OPCode* JassVM::get_cur_opcode() {
	return this ? jass_vm.opcode_struct : nullptr;
}

uint32_t JassVM::get_jass_offset(OPCode* code) {
	if (!this || !code)
		return 0;
	auto* begin = get_symbol_table() ? get_symbol_table()->get_jass_begin() : nullptr;
	if (!begin)
		return 0;
	return static_cast<uint32_t>((reinterpret_cast<uintptr_t>(code) - reinterpret_cast<uintptr_t>(begin)) / 4);
}

JassRegister* JassVM::get_variable(const string& name, bool is_local) {
	ScriptDataTable* table = is_local ? get_stack_frame()->get_local_table() : get_global_table();
    if (!table) return nullptr;
    // ScriptDataTable 通过 getJassVariable 查找
    return call_this<JassRegister*>(getJassVariable, table, name.c_str());
}

int32_t JassVM::exec_code(const OPCode* code) {
	if (!this || !code)
		return 0;

	call_this<uint32_t>(executeCode, this, code, 0, 0, 300000, 1, 1);
	return (int32_t)get_register(0)->value;
}

int32_t JassVM::exec_code(const string& code) {
	JassFuncStruct* jass_func = get_jass_func(code);
	return jass_func ? exec_code(jass_func->func_addr) : 0;
}

int32_t JassVM::exec_code(jCode code) {
	if (!this)
		return 0;

	intptr_t code_table = jass_vm.code_table;
	if (code_table <= 0)
		return 0;

	uint32_t size = *(uint32_t*)(code_table + 0x4);
	if (code >= size) {
		auto& closure = cpp_closure::get(code);
		if (closure)
			return closure();
	}

	OPCode** code_array = *(OPCode***)(code_table + 0x8);
	if ((intptr_t)code_array <= 0)
		return 0;

	return exec_code(code_array[code]);
}

uint32_t JassVM::to_jCode(const OPCode* opcode) {
    if (opcode == nullptr)
        return 0;
	return call_this<uint32_t>(codeToIndex, this, opcode);
}

jCode JassVM::to_jCode(const string& func_name) {
	auto func = get_jass_func(func_name);
	if (!func)
		return 0;
	return to_jCode(func->func_addr);
}

OPCode* JassVM::to_opcode(jCode code) {
    if (!this) return nullptr;
    intptr_t table = jass_vm.code_table;
    if (!table) return nullptr;
    uint32_t size = *(uint32_t*)(table + 0x4);
    if (code >= size)
        return nullptr;
    OPCode** arr = *(OPCode***)(table + 0x8);
    if (!arr) return nullptr;
    return arr[code];
}

uint32_t JassVM::get_jass_array_value(const JassRegister& var, uint32_t index) {
	if (!&var)
		return 0;

	const Parser::VTYPE type = static_cast<Parser::VTYPE>(var.type_0x18);
	if (type < Parser::VT_INTEGER_ARRAY)
		return 0;

	JassArray* arr = (JassArray*)var.value;
	if (!arr)
		return 0;

	return call_this<uint32_t>(getJassArrayValue, var.value, index);
}

uint32_t JassVM::get_jass_array_value(const string& name, uint32_t index, bool is_local) {
	auto* table = is_local ? get_stack_frame()->get_local_table() : get_global_table();
	if (!table)
		return 0;
	auto* var = table->find(name);
	return var ? get_jass_array_value(*var, index) : 0;
}

bool JassVM::set_jass_array_value(JassRegister& var, uint32_t index, uint32_t value) {
	if (!&var)
		return false;

	const Parser::VTYPE type = static_cast<Parser::VTYPE>(var.type_0x18);
	if (type < Parser::VT_INTEGER_ARRAY)
		return false;

	JassArray* arr = (JassArray*)var.value;
	if (arr) {
		uint32_t old_value = call_this<uint32_t>(getJassArrayValue, arr, index);
		if (old_value != value) {
			if (type == Parser::VT_HANDLE_ARRAY)
				unref_handle((jHandle)old_value);
			else if (type == Parser::VT_STRING_ARRAY)
				call_this<void>(processStringOldValue, this, old_value);
		}
	}

	call_this<void>(setJassArrayValue, &var, type - 0x5, index, value, jass_vm.check_type);
	return true;
}

bool JassVM::set_jass_array_value(const string& name, uint32_t index, uint32_t value, bool is_local) {
	auto* table = is_local ? get_stack_frame()->get_local_table() : get_global_table();
	if (!table)
		return false;
	auto* var = table->find(name);
	return var ? set_jass_array_value(*var, index, value) : false;
}

OPCode* JassVM::opcode_jump(uint32_t line) {
	auto ptr = get_field<uintptr_t>(0x2870);
	if (!ptr)
		return nullptr;
	auto arr = *(OPCode***)(ptr + 0x8);
	if (!arr)
		return nullptr;
	return arr[line];
}

bool JassVM::opcode_movrl(JassRegister& reg, uint32_t value, uint32_t type, JassVM* jvm) {
	return &reg
		? call_this<BOOL>(opcodeMOVRL, &reg, type, value, jvm)
		: false;
}

bool JassVM::handle_ref(jHandle h, bool is_ref) {
	if (!jass::is_handle_valid(h))
		return false;
	call_fast<void>(reinterpret_cast<uintptr_t>(jass_vm.set_handle_ref), h, is_ref ? 0 : 1, jass_vm.map_table);
	return true;
}

// 转换jString为RCString (JassVM成员函数)
const RCString* JassVM::to_RCString(jString sh) {
	auto* table = get_string_table();
	if (!table || sh >= table->size)
		return nullptr;
	
	auto* arr = table->arr;
	if (!arr)
		return nullptr;
	
	return &arr[sh].str;
}

// 转换jString为CString (JassVM成员函数)
const char* JassVM::to_CString(jString sh) {
	auto* table = get_string_table();
	if (!table || sh >= table->size)
		return "";
	
	auto* arr = table->arr;
	if (!arr)
		return "";
	
	auto* rep = arr[sh].str.string_rep;
	if (!rep)
		return "";
	
	const char* res = rep->str;
	if (!res)
		return "";
	
	return res;
}

string JassVM::arg_to_str_opcode(OPCode* value) {
	if (!this)
		return "[Error jvm]";
	if (!value)
		return "[Error code addr]";

	--value;
	if (value->op != Parser::OPCODE_FUNCTION)
		return "ANONYMOUS";

	auto* symbol = get_symbol_table() ? get_symbol_table()->get_symbol(value->arg) : nullptr;
	if (!symbol || !symbol->name)
		return "[Error code addr]";
	++value;
	return symbol->name;
}

string JassVM::arg_to_str_jCode(jCode value) {
	auto& closure = cpp_closure::get(value);
	if (closure)
		return string("MHCL-") + closure.get_name();

	uintptr_t ptr = jass_vm.code_table;
	if (!ptr)
		return "[Error jCode]";
	if (value >= *(uint32_t*)(ptr + 0x4))
		return "[Error jCode]";
	ptr = *(uintptr_t*)(ptr + 0x8);
	if (!ptr)
		return "[Error jCode]";
	return arg_to_str_opcode(((OPCode**)ptr)[value]);
}

string JassVM::arg_to_str_jHandle(jHandle value) {
	return arg_to_str_object(convert::to_object<void*>(value), value);
}

string JassVM::arg_to_str_object(void* value) {
	return arg_to_str_object(value, convert::inquire_handle(value));
}

string JassVM::arg_to_str_object(void* value, jHandle handle) {
	uint32_t type = jass::get_obj_type(value);
	std::ostringstream ss;
	ss << "[" << convert::type_to_str(type) << "]"
	   << std::hex << std::setw(6) << std::setfill('0') << handle;
	return ss.str();
}

// JassStackFrame 实现
bool JassStackFrame::valid() const { return (intptr_t)this > 0; }
JassStackFrame::operator bool() const { return valid(); }
uint32_t JassStackFrame::get_top() { return valid() ? stack_frame.top_0x8C : 0; }
uint32_t JassStackFrame::get_top_not_safe() { return stack_frame.top_0x8C; }
bool JassStackFrame::set_top(uint32_t top) { if(valid()) { stack_frame.top_0x8C = top; return true; } return false; }
JassStackFrame* JassStackFrame::get_last_stack() { return valid() ? (JassStackFrame*)stack_frame.last_stack : nullptr; }
ScriptDataTable* JassStackFrame::get_local_table() { return valid() ? &stack_frame.local_table : nullptr; }
JassRegister* JassStackFrame::get_frame(uint32_t index) { return valid() ? stack_frame.frame[index] : nullptr; }
JassRegister* JassStackFrame::get_frame_not_safe(uint32_t index) { return stack_frame.frame[index]; }
JassRegister* JassStackFrame::get_variable(const string& name) { 
    return get_local_table() ? call_this<JassRegister*>(getJassVariable, get_local_table(), name.c_str()) : nullptr;
}

string JassStackFrame::to_string(JassVM* jvm, uint32_t begin, uint32_t count) {
	if (!valid() || !jvm)
		return "";
	const uint32_t top = get_top_not_safe();
	count = std::min(count, top - begin + 1);

	std::ostringstream ss;
	for (uint32_t i = 0; i < count; ++i) {
		auto& frame = *get_frame_not_safe(begin + i);
		uint32_t value = (uint32_t&)frame.value;
		if (i)
			ss << ", ";

		switch (frame.type_0x18) {
			case Parser::VT_CODE: ss << jvm->arg_to_str_opcode((OPCode*)value); break;
			case Parser::VT_INTEGER: ss << static_cast<int32_t>(value); break;
			case Parser::VT_REAL: {
				float f;
				std::memcpy(&f, &value, sizeof(float));
				ss << f;
			} break;
			case Parser::VT_STRING: ss << "\"" << jvm->to_CString(value) << "\""; break;
			case Parser::VT_HANDLE: ss << jvm->arg_to_str_jHandle(value); break;
			case Parser::VT_BOOLEAN: ss << (value ? "true" : "false"); break;
			default: ss << "[type:" << frame.type_0x18 << " val:" << value << "]"; break;
		}
	}
	return ss.str();
}

void JassStackFrame::dump(JassRegister** res, uint32_t count) {
	if (!valid() || !res)
		return;
	for (uint32_t i = 0; i < count; ++i)
		res[i] = get_frame_not_safe(i);
}

void JassStackFrame::dump(void** res, uint32_t count) {
	if (!valid() || !res)
		return;
	for (uint32_t i = 0; i < count; ++i) {
		auto* frame = get_frame_not_safe(i);
		res[i] = frame ? frame->value : nullptr;
	}
}

void JassStackFrame::dump_with_type(void* res, uint32_t count, uint32_t offset) {
	if (!valid() || !res)
		return;
	auto* buf = reinterpret_cast<uint8_t*>(res);
	for (uint32_t i = 0; i < count; ++i) {
		auto* frame = get_frame_not_safe(i);
		if (!frame)
			continue;
		uint32_t* dst = reinterpret_cast<uint32_t*>(buf + offset * i);
		dst[0] = (uint32_t)frame->value;
		dst[1] = frame->type_0x18;
		dst[2] = frame->type_0x1C;
	}
}

JassRegister* ScriptDataTable::find(const string& name) {
	if (!this || !name.size() || !getJassVariable)
		return nullptr;
	return call_this<JassRegister*>(getJassVariable, this, name.c_str());
}

// JassSymbolTable 实现
OPCode* JassSymbolTable::get_jass_begin() { return this ? symbol_table.start_line : nullptr; }
uint32_t JassSymbolTable::get_jass_offset(OPCode* cur_opcode) {
	uintptr_t start_line = reinterpret_cast<uintptr_t>(get_jass_begin());
	if (!start_line || !cur_opcode)
		return 0;
	return static_cast<uint32_t>((reinterpret_cast<uintptr_t>(cur_opcode) - start_line) >> 2);
}
JassSymbol* JassSymbolTable::find(const string& symbol_name) {
	if (!this || !symbol_table.table || !symbol_table.table->symbol_array)
		return nullptr;
	JassSymbol** table = symbol_table.table->symbol_array;
	uint32_t index = 1;
	JassSymbol* node = table[index];
	while (node) {
		if (node->name && symbol_name == node->name)
			return node;
		++index;
		node = table[index];
	}
	return nullptr;
}
JassSymbol* JassSymbolTable::get_symbol(uint32_t index) {
    if (!this || !symbol_table.table || !symbol_table.table->symbol_array) return nullptr;
    return symbol_table.table->symbol_array[index];
}
const char* JassSymbolTable::get_symbol_name(uint32_t index) {
	auto* symbol = get_symbol(index);
	return symbol ? symbol->name : nullptr;
}
JassSymbol* JassSymbolTable::add_symbol(const string& symbol_name) {
    if (!this) return nullptr;
    call_this<const char*>(addSymbol, symbol_table.table, symbol_name.c_str(), 0);
    return find(symbol_name);
}


// 初始化
void jass_init() {
	if (pGameDLL) {
		switch (game_version)
		{
		case(0x27a):
			getDataNode				= pGameDLL + 0x1C3200;
			getInstance				= pGameDLL + 0x04EFB0;
			getJassVM				= pGameDLL + 0x7E1100;
			initJassVM				= pGameDLL + 0x7ECCB0;
			getJassFuncStruct		= pGameDLL + 0x7EFBB0;
			getJassVariable			= pGameDLL + 0x7E5820;
			executeFunc				= pGameDLL + 0x7F2940;
			executeCode				= pGameDLL + 0x7F2B40;
			codeToIndex				= pGameDLL + 0x7ECE50;
			getInstanceGenerator	= pGameDLL + 0x0219F0;
			generateInstance		= pGameDLL + 0x038B20;
			generateAgent			= pGameDLL + 0x1012B0;
			getAgentHashKey			= pGameDLL + 0x17A710;
			getAgentDataNode		= pGameDLL + 0x0352A0;
			getUIDefData			= pGameDLL + 0x327020;
			getCObject				= pGameDLL + 0x037350;
			getCAgent				= pGameDLL + 0x044150;
			setCAgent				= pGameDLL + 0x206680;
			isCAgentValid			= pGameDLL + 0x037810;
			checkInherit			= pGameDLL + 0x02FBF0;
			unrefObj				= pGameDLL + 0x04F1A0;
			orderIdToString			= pGameDLL + 0x1C2800;
			getCurEventId			= pGameDLL + 0x1E5D90;
			getEventDataCall1		= pGameDLL + 0x1E5DB0;
			getEventDataCall2		= pGameDLL + 0x1E5D70;
			opcodeMOVRL				= pGameDLL + 0x7EC660;
			getJassArrayValue		= pGameDLL + 0x7ED050;
			setJassArrayValue		= pGameDLL + 0x7EC6E0;
			processStringOldValue	= pGameDLL + 0x7EC9B0;
			addSymbol				= pGameDLL + 0x7DEAF0;
			copyThread				= pGameDLL + 0x7E2F40;

			jassParser = pGameDLL + 0x7F1A20;
			pushReturn = pGameDLL + 0x7F3020;

			cpp_closure::jump_in = pGameDLL + 0x1DA9A0;
			break;
		default:
			return;
		}

        // JASS 执行入口会触发后续初始化（game_table / NetEventHook 等），
        // 这里不再强制调用，以免过早访问未就绪对象。
        OutputDebugStringA("[Jass] jass_init: Function pointers resolved. Waiting for JASS execution...\n");
	} else {
        OutputDebugStringA("[Jass] pGameDLL is null, skipping jass_init\n");
    }
}
