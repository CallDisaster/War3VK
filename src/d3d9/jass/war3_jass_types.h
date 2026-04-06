#pragma once
#include "war3_types.h"
#include "war3_game_struct.h"
#include "war3_jass_parser.h"

namespace jass {
	using jCode = uint32_t;
	using jHandle = uint32_t;
	using jString = uint32_t;
}

using jass::jCode;
using jass::jHandle;
using jass::jString;

// Forward declarations
class Unit;
class Ability;
class TriggerWar3;

class HandleId;
class JassVM;
class JassStackFrame;
class JassSymbolTable;

// Jass虚拟机类
class JassVM {

private:
	JassVMStruct jass_vm;

public:
	static const CStringRep	NULL_CSTRINGREP;
	static const RCString	NULL_RCSTRING;

public:
	// 获取字段值
	template <typename R>
	R			get_field(uint32_t off) { return this ? *(R*)((uintptr_t)this + off) : (R)0; }
	// 获取字段地址
	template <typename R>
	R*			lea_field(uint32_t off) { return this ? (R*)((uintptr_t)this + off) : nullptr; }
	// 设置字段值
	template <typename T>
	bool		set_field(uint32_t off, T value) { 
		if (this) {
			*(T*)((uintptr_t)this + off) = value;
			return true;
		}
		return false;
	}

	// 获取虚拟机结构
	JassVMStruct*		get_vm();
	// 获取寄存器
	JassRegister*		get_register(uint32_t index);
	// 获取栈帧
	JassStackFrame*		get_stack_frame();
	// 获取符号表
	JassSymbolTable*	get_symbol_table();
	// 获取全局变量表
	ScriptDataTable*	get_global_table();
	// 获取字符串表
	JassStringTable*	get_string_table();
	// 获取字符串数量
	uint32_t			get_string_count();
	// 获取jass函数结构
	JassFuncStruct*		get_jass_func(const string& func_name);
	// 获取当前code
	OPCode*				get_cur_opcode();
	// 获取jass函数偏移
	uint32_t			get_jass_offset(OPCode* code);
	// 获取变量
	JassRegister*		get_variable(const string& name, bool is_local = false);
	// 执行code
	int32_t				exec_code(const OPCode* code);
	// 执行code
	int32_t				exec_code(const string& code);
	// 执行code
	int32_t				exec_code(jCode code);

	// 转换code为jCode
	jCode				to_jCode(const OPCode* opcode);
	// 转换函数名为jCode
	jCode				to_jCode(const string& func_name);
	// 转换jCode为opcode
	OPCode*				to_opcode(jCode code);
	// 获取数组数值
	uint32_t			get_jass_array_value(const JassRegister& var, uint32_t index);
	// 获取数组数值
	uint32_t			get_jass_array_value(const string& name, uint32_t index, bool is_local = false);
	// 设置数组数值
	bool				set_jass_array_value(JassRegister& var, uint32_t index, uint32_t value);
	// 设置数组数值
	bool				set_jass_array_value(const string& name, uint32_t index, uint32_t value, bool is_local = false);
	// 获取jmp目标点
	OPCode*				opcode_jump(uint32_t line);
	// 字面值写入寄存器
	bool				opcode_movrl(JassRegister& reg, uint32_t value, uint32_t type, JassVM* jvm = nullptr);

	// 引用句柄
	bool				handle_ref(jHandle h, bool is_ref);

	// 转换jString为RCString
	const RCString*		to_RCString(jString sh);

	// 转换jString为CString
	const char*			to_CString(jString sh);

	// 转换opcode为字符串
	string				arg_to_str_opcode(OPCode* value);

	// 转换jCode为字符串
	string				arg_to_str_jCode(jCode value);

	// 转换jHandle为字符串
	static string		arg_to_str_jHandle(jHandle value);

	// 转换object为字符串
	static string		arg_to_str_object(void* value);

	// 转换object为字符串
	static string		arg_to_str_object(void* value, jHandle handle);
};

// Jass栈帧类
class JassStackFrame {

private:
	JassStackStruct		stack_frame;

	struct ParamStruct {
		uint32_t		value;
		uint32_t		type;
		uint32_t		handle_type;
	};

public:

	// 有效
	bool				valid() const;

	// 有效
	explicit operator	bool() const;

	// 获取栈顶
	uint32_t			get_top();
	// 获取栈顶
	uint32_t			get_top_not_safe();
	// 设置栈顶
	bool				set_top(uint32_t top);
	// 获取上一栈帧
	JassStackFrame*		get_last_stack();
	// 获取局部变量表
	ScriptDataTable*	get_local_table();

	// 获取帧
	JassRegister*		get_frame_not_safe(uint32_t index);

	// 获取帧
	JassRegister*		get_frame(uint32_t index);

	// 获取局部变量
	JassRegister*		get_variable(const string& name);

	// 转换为字符串
	string				to_string(JassVM* jvm, uint32_t begin, uint32_t count = -1);

	// Dump栈元素
	void				dump(JassRegister** res, uint32_t count);
	// Dump栈元素值
	void				dump(void** res, uint32_t count);
	// Dump栈元素值到param_dump
	void				dump_with_type(void* res, uint32_t count, uint32_t offset);
};

// Jass符号表类
class JassSymbolTable {

private:
	JassSymbolStruct	symbol_table;

public:

	// 获取jass脚本起始位置
	OPCode*				get_jass_begin();
	// 获取jass语句偏移
	uint32_t			get_jass_offset(OPCode* cur_opcode);

	// 查找表项
	JassSymbol*			find(const string& symbol_name);
	// 查找表项
	JassSymbol*			get_symbol(uint32_t index);
	// 获取符号名
	const char*			get_symbol_name(uint32_t index);

	// 添加符号
	JassSymbol*			add_symbol(const string& symbol_name);
};

namespace jass {
	// jass函数形参信息
	using param_t		= Parser::VTYPE;
	struct param_info_t {
		std::vector<param_t>	param_type;
		param_t					ret_type = Parser::VT_NOTHING;
	};


	// c++函数包装为code
	namespace cpp_closure {
		using callback_t = std::function<int32_t()>;

		// 闭包
		using closure_t = class _Closure_Info {
		public:
			// 获取code
			jCode get_code() const;

			// 获取名称
			const string& get_name() const;

			// 能引起异步
			BOOL can_cause_desync() const;

			// 调用
			uint32_t operator()() const;

			// 有效
			bool valid() const;
			explicit operator bool() const;

			// 判等
			bool operator==(const _Closure_Info& that) const;
			bool operator!=(const _Closure_Info& that) const;

			// 计算哈希
			static uint32_t HASH(const string& str);
		public:
			_Closure_Info(jCode _code, const string& _name, const callback_t& _callback, BOOL _cause_desync)
				: code(_code)
				, name(_name)
				, callback(_callback)
				, cause_desync(_cause_desync)
			{}
		private:
			const jCode			code;		// 通过名称哈希得到 唯一标识符
			const string		name;
			const callback_t	callback;
			BOOL				cause_desync;
		};

		using closure_stack_t = std::vector<const closure_t*>;

		// 生成闭包
		const closure_t& fast_call	make(const string& name, const callback_t& callback, bool cause_desync = true);
		// 获取闭包
		const closure_t&			get(jCode code);
		// 获取闭包
		const closure_t&			get(const string& name);
		// 获取当前闭包栈
		const closure_stack_t&		get_stack();
		// 获取当前闭包帧
		const closure_t&			get_frame();
	}

	// code带参闭包
	namespace code_closure {
		constexpr uint32_t MAX_PARAM_COUNT = 16;
		using param_buffer_t = std::array<uint32_t, MAX_PARAM_COUNT>;

		// 闭包信息
		typedef struct ClosureInfo {
			jCode					j_code;		// 生成的code 唯一标识符
			string					name;
			std::vector<OPCode>		closure;	// 闭包code
			param_info_t			param_info;
			std::vector<uint32_t>	positions;	// 参数在字节码中的位置

			// 禁止隐式转换
			explicit operator bool() const;

			bool operator==(const ClosureInfo& that) const;

			bool operator!=(const ClosureInfo& that) const;

			int32_t operator()(const param_buffer_t& args) const;
			
			int32_t operator()() const;

			static uint32_t hash(const string& str);
		} closure_info_t;

		// code默认前缀
		inline const string DEFAULT_PREFIX = "_MHCODE_";

		// 生成闭包
		const closure_info_t& __fastcall	make(const string& name, const param_info_t& param_info, const string& prefix = DEFAULT_PREFIX);
		// 获取闭包
		const closure_info_t&				get(jCode code);
		// 获取闭包
		const closure_info_t&				get(const string& name);
	}

	// handle节点
	using handle_node_t = struct _Handle_Node {
		uint32_t	ref_count;
		void*		obj;
	};
	using handle_list_t = std::vector<handle_node_t>;

	// 获取实例
	extern void*					get_instance(uint32_t id);
	// 获取jass虚拟机
	JassVM*								get_jass_vm(uint32_t index);
	// 获取主线程
	JassVM*								get_main_thread();
	// 获取当前线程
	JassVM*								get_cur_thread();
	// 获取回调线程
	JassVM* fast_call					get_callback_thread();
	// 执行code
	int32_t	fast_call					exec_code(const OPCode* code);
	// 执行code
	int32_t	fast_call					exec_code(const string& code);
	// 执行code
	int32_t	fast_call					exec_code(jCode code);
	// 获取数据节点
	void*								get_data_node();
	// 创建agent
	void* fast_call					create_agent(uint32_t id, uint32_t param = 0);
	// 获取Agent哈希键
	uint32_t							get_agent_hash_key(uint32_t id);
	// 获取Agent数据节点
	void*								get_agent_data_node(uintptr_t dataNode, uint32_t aid);
	// 获取UI数据节点
	void*								get_ui_data_node(uint32_t id);
	// 获取CObject
	void*								get_object(uint32_t hash1, uint32_t hash2);
	// 获取CObject
	void*								get_object(HashGroup const& hash);
	// 获取CAgent
	void*								get_agent(uint32_t hash1, uint32_t hash2);
	// 获取CAgent
	void*								get_agent(HashGroup const& hash);
	// 设置CAgent
	void								set_agent(HashGroup& hash, void* obj);
	// agent有效
	bool								is_agent_valid(void* obj);
	// 检查继承关系
	bool								check_inherit(uint32_t child_class, uint32_t parent_class);
	// 检查继承关系
	bool								check_inherit(void* obj, uint32_t parent_class);
	// 引用handle
	bool								ref_handle(jHandle h);
	// 解引用handle
	bool								unref_handle(jHandle h);
	// 解引用对象
	void*								unref_obj(void* obj);
	// 获取对象类型
	uint32_t							get_obj_type(void* obj);
	// 获取handle类型
	uint32_t							get_handle_type(jHandle handle);
	// 对象是widget
	bool								is_obj_widget(void* obj);
	// handle是有效的
	bool								is_handle_valid(jHandle handle);
	// 转换命令id为字符串
	char*								order_to_string(uint32_t order);
	// 获取当前原生事件ID
	uint32_t							get_event_id();
	// 获取原生事件数据
	uintptr_t							get_event_data();
	// 替换原生字符串
	bool fast_call						replace_string(const char** addr, const char* str);
	// 恢复原生字符串
	bool fast_call						reset_string(const char** addr);
	// 获取handle列表
	handle_list_t						get_handle_list();
	// 获取峰值handle数量
	uint32_t							get_max_handle_count();
}

void jass_init();
