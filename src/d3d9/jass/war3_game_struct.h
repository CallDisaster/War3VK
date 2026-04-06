#pragma once
#include "war3_types.h"

struct FloatStruct;
struct FloatMini;
struct FloatMiniEx;
struct CAgentTimer;
struct CAgentTimerExtended;
struct SplashData;
struct CSplashData;

struct GameTimer;

struct CGameWar3;
struct CGameState;
struct CDoodads;
struct CWorldObjects;

struct JassThreadLocal;
struct JassVMStruct;
struct JassRegister;

struct JassStringTable;
struct JassHandleTable;
struct JassStackStruct;
struct JassSymbolStruct;

struct OPCode;
struct JassFuncStruct;
struct NativeFuncStruct;

struct CGxDevice;
struct CGxuLight;
struct GxStagePresetRecord;
struct CModelPartStateController;
struct CPlaneParticleEmitter;
struct SceneNodeChildBucket;
struct CGeoset;
struct CGeosetData;
struct CModel;
struct CModelAnimController;
struct CModelInstance;
struct CModelData;

struct CDataStore;

struct CSpriteUber;
struct CSpriteMini;

struct CTriggerWar3;

struct CUnit;
struct CPreselectUI;
struct UnitDefData;
struct UnitUIDefData;

struct CSprite;

struct CAbility;
struct CAbilityAttack;
struct CAbilityMove;
struct CAbilityHero;
struct CAbilityBuild;
struct CAbilityInventory;
struct AbilityDefData;
struct AbilityUIDefData;
struct AbilityLevelData;

struct CPlayerWar3;
struct CSelectionWar3;


struct CGroup;
struct CUnitSet;

struct CItem;

struct CEffect;

struct DamageEventData;

struct CSpriteClickEvent;
struct CSpriteTrackEvent;
struct CTerrainClickEvent;
struct CMiniMapClickEvent;

struct CMouseEvent;

struct COrder;

struct CFramePointAbsolute;
struct CFramePointRelative;

struct CWar3Image;
struct CObserver;
struct CLayer;
struct CFrame;

struct SimpleTopButtonList;
struct SimpleTopButtonListArray;
struct CSimpleTop;
struct CSimpleFrame;

struct CSimpleRegion;
struct CSimpleTexture;
struct SimpleFrameTextureSettings;

struct CGameUI;

struct CTargetMode;
struct CSelectMode;
struct CDragSelectMode;
struct CDragScrollMode;
struct CBuildMode;
struct CBuildFrame;
struct CSignalMode;
struct CEscMenu;

struct CGlueCheckBoxWar3;

struct CSuspendIndicator;
struct CUnresponsiveIndicator;
struct CAllianceMode;
struct CAllianceDialog;
struct CAllianceSlot;
struct CChatMode;
struct CQuestMode;
struct CScriptDialogMode;

struct CSelecetionManager;
struct CDragScrollManager;
struct CCamera;
struct CCameraWar3;

struct CMultiBoard;

struct CWorldFrameWar3;
struct CMiniMap;
struct CInfoBar;
struct CCommandBar;
struct CResourceBar;
struct CUpperButtonBar;
struct CHeroBar;
struct CPeonBar;
struct CSimpleMessageFrame;
struct CPortraitButton;
struct CTimeOfDayIndicator;
struct CChatEditBar;
struct CCinematicPanel;

struct CCommandButton;
struct CCommandButtonData;

struct TEXTBLOCK;

struct CLayoutFrame;
struct CModelFrame;
struct CBackDropFrame;
struct CEditBox;
struct CSimpleButton;
struct CSimpleConsole;
struct CSimpleFontString;
struct CSimpleStatusBar;
struct CSlider;
struct CSpriteFrame;
struct CStatBar;
struct CTextArea;
struct CHighlightFrame;
struct CControl;
struct CTextFrame;


struct Integer;
struct HashGroup;



struct RCString;
struct CStringRep;


// 魔兽数组
template <typename T>
struct GameArray {
	byte_t				ukn_0x0[0x4];
	uint32_t			capacity;				// 0x4
	T*					arr;					// 0x8
	uint32_t			size;					// 0xC
}; // sizeof = 0x10

// 魔兽数组2
template <typename T>
struct GameArray2 {
	uint32_t			capacity;				// 0x0
	uint32_t			size;					// 0x8
	T*					arr;					// 0x4
	byte_t				ukn_0xC[0x4];
}; // sizeof = 0x10

// 魔兽数组
template <typename T>
struct GameSimpleArray {
	uint32_t			capacity;				// 0x0
	uint32_t			size;					// 0x4
	T*					arr;					// 0x8
}; // sizeof = 0xC

// 魔兽栈
template <typename T>
struct GameStack {
	uint32_t			capacity;				// 0x0
	uint32_t			top;					// 0x4
	T*					arr;					// 0x8
};

template <typename T>
struct War3CompactArrayHeader {
	uint32_t			capacity;				// 0x0
	uint32_t			count;					// 0x4
	T*					data;					// 0x8
}; // sizeof = 0xC

template <typename T>
struct War3GrowableArrayHeader {
	uint32_t			capacity;				// 0x0
	uint32_t			count;					// 0x4
	T*					data;					// 0x8
	uint32_t			growth_alignment;		// 0xC
}; // sizeof = 0x10


struct RCString {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	const CStringRep*	string_rep;				// 0x8

	const char*			get_str() const;
	bool				set_str(const char* str);

	RCString(const CStringRep& rep);
	RCString();
}; // sizeof = 0xC

struct CStringRep {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	int32_t				string_hash;			// 0x8
	unsigned char		ukn_0xC[0x4];
	const CStringRep*	next_string;			// 0x10 | 待确定
	unsigned char		ukn_0x14[0x8];
	const char*			str;					// 0x1C

	const char*			get_str() const;

	CStringRep(const char* _str);
	CStringRep();
};

struct CObserver {
	void**						vf_table;				// 0x0
	uint32_t					ref_count;				// 0x4
	uint32_t					registry;				// 0x8 ObserverRegistry 
};// sizeof = 0xC

struct HashGroup {
	uint32_t			hash_0x0;				// 0x0
	uint32_t			hash_0x4;				// 0x4

	HashGroup(uint32_t hash1 = 0xFFFFFFFF, uint32_t hash2 = 0xFFFFFFFF)
		: hash_0x0(hash1)
		, hash_0x4(hash2)
	{}

	bool is_exist() { return !!this; }

	bool is_valid() { return (is_exist() && ((int32_t)hash_0x0 > 0) && ((int32_t)hash_0x4 > 0)); }

	bool operator==(const HashGroup& rhs) const {
		return hash_0x0 == rhs.hash_0x0
			&& hash_0x4 == rhs.hash_0x4;
	}
};

struct HashCargo;
struct HashCargo {
	HashGroup			hash;					// 0x0
	unsigned char		ukn_0x8[0x4];			// = 0
};

struct Integer {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	HashGroup			hash;					// 0x8
}; // sizeof = ?

struct FloatMini {
	void**				vf_table;				// 0x0
	float				value;					// 0x4

	float get_value() { return this ? value : 0.f; }
	bool set_value(float value_) {
		if (this) {
			value = value_;
			return true;
		} 
		return false;
	}
};

struct FloatStruct {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	HashGroup			hash;					// 0x8
}; // sizeof = 0x10

struct FloatMiniEx {
	void**				vf_table;
	float				value;					// 0x4
	unsigned char		ukn_0x8[0x8];
};

struct SplashData {
	uint32_t			target_allow;
	FloatMini			half_factor;
	FloatMini			small_factor;
	FloatMini			full_area;
	FloatMini			half_area;
	FloatMini			small_area;
};

struct CSplashData {
	void**				vf_table;
	uint32_t			ref_count;
	unsigned char		ukn_0x8[0x18];
	SplashData			splash_data;
};

struct CTimerWar3;
struct AgentTimerDispatch;
struct AgentTimerInfo;

struct AgentTimerDispatch {
	unsigned char		ukn_0x0[0x40];
	float				cur_stamp;				// 0x40
};

struct AgentTimerInfo {
	unsigned char		ukn_0x0[0x4];
	float				time_out;				// 0x4
	float				time_length;			// 0x8
	AgentTimerDispatch* timer_dispatch;			// 0xC
	uint32_t			flag_0x10;				// 0x10
	unsigned char		ukn_0x14[0x4];
	CAgentTimer*		timer;					// 0x18
	uint32_t			callback_id;			// 0x1C
};

struct CAgentTimer {
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x4];
	void*				obj;					// 0x8 挂计时器的物体
	AgentTimerInfo*		running_info;			// 0xC
	unsigned char		ukn_0x10[0x4];
}; // sizeof = 0x14

struct AgentTimerExtendedInfo {
	unsigned char		ukn_0x0[0x4];
	float				end_stamp;				// 0x4 结束的时间戳
	float				duration;				// 0x8
	void*				central_timer;			// 0xC 中心计时器, +0x40为当前时间戳
};

struct CAgentTimerExtended {
	void**						vf_table;				// 0x0
	unsigned char				ukn_0x4[0x4];
	void*						obj;					// 0x8 计时器对象
	AgentTimerExtendedInfo*		running_info;			// 0xC

};// sizeof = ?

struct CTimerWar3 {
	void**						vf_table;				// 0x0
	uint32_t					ref_count;				// 0x4
	unsigned char				ukn_0x8[0x4];
	HashGroup					agent_hash;				// 0xC
	CObserver					observer;				// 0x14
	unsigned char				ukn_0x20[0x4];
	CAgentTimerExtended			timer;
	uint32_t					flag_0x34;				// 0x34
	unsigned char				ukn_0x38[0x8];
	void*						callback;				// 0x40
	FloatMini					interval;				// 0x44
	FloatMini					p_remain;				// 0x4C 暂停时存储上次剩余时间
}; // sizeof = 0x50?

struct UnitSetDataNode;

struct UnitSetDataNode {
	UnitSetDataNode*	last_node;				// 0x0
	UnitSetDataNode*	next_node;				// 0x4
	CUnit*				unit;					// 0x8
};

struct CUnitSet {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	UnitSetDataNode*	unit_list_end;			// 0x8
	UnitSetDataNode*	unit_list_start;		// 0xC
	uint32_t			count;					// 0x10
}; // sizeof = 0x14

struct CGroup {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	byte_t				ukn_0x8[0x1C];
	CUnitSet			unit_set;				// 0x24
	byte_t				ukn_0x38[0xC];
	HashGroup			filter;
};


struct SmartPosition {
	void**				vtable;					// 0x0
	uint32_t			refCount;				// 0x4
	HashGroup			pos_hash;				// 0x8
};//sizeof = 0x10


struct ScriptDataTable;
struct JassArray;


struct JassThreadLocal {
	unsigned char		ukn_0x0[0x14];
	uint32_t			thread_id;				// 0x14
	uintptr_t			native_func_table;		// 0x18
};

struct JassRegister {
	JassRegister*		next_frame;				// 0x0 只在栈元素中，链表结构
	unsigned char		ukn_0x4[0x10];
	const char*			name;					// 0x14 名称 在变量中
	uint32_t			type_0x18;				// 0x18 寄存器类型	该类型为0(Nothing)的话进行PUSH操作即变量未初始化就使用
	uint32_t			type_0x1C;				// 0x1C	字节码赋予的类型
	void*				value;					// 0x20 数组这里是指向table的指针
	uint32_t			is_local;				// 0x24 是局部变量

	bool valid() const;

	explicit operator bool() const;

};//sizeof = 0x28

struct JassVMStruct
{
	unsigned char		ukn_0x0[0x20];
	OPCode*				opcode_struct;			// 0x20
	char				ukn_0x24[0x10];
	uint32_t			has_sleep;				// 0x34
	unsigned char		ukn_0x38[0xC];
	int32_t				oplimit;				// 0x44
	uint32_t			check_type;				// 0x48
	unsigned char		ukn_0x4C[0x4];
	JassRegister		reg[0x100];				// 0x50
	uint32_t			index;					// 0x2850
	uint32_t			ukn3;
	JassSymbolStruct*	symbol_table;			// 0x2858
	ScriptDataTable*	global_table;			// 0x285C
	unsigned char		ukn_0x2860[0x8];
	JassStackStruct*	stack_struct;			// 0x2868
	unsigned char		ukn_0x286C[0x8];
	JassStringTable*	string_table;		    // 0x2874
	unsigned char		ukn_0x2878[0xC];
	uintptr_t			func_table;				// 0x2884
	uint32_t			code_table;				// 0x2888
	unsigned char		ukn_0x288C[0x14];
	void*				set_handle_ref;			// 0x28A0
	uintptr_t			map_table;				// 0x28A4	CGameState**
}; // sizeof = 0x28A8

struct JassStringTableNode {
	RCString			str;					// 0x0
	uint32_t			ukn_0xC;				// 0xC
}; // sizeof = 0x10

struct JassStringTable {
	uint32_t				capacity;
	uint32_t				size;
	JassStringTableNode*	arr;
};

struct JassHandleNode {
	uint32_t			ref_count;				// 0x0
	void*				object;					// 0x4
	unsigned char		ukn_0x8[0x4];
};

struct JassHandleTable {
	uint32_t			capacity;				// 0x0
	uint32_t			size;					// 0x4
	JassHandleNode*		handle_array;			// 0x8
	byte_t				ukn_0xC[0x4];
}; // sizeof = 0x10

struct ScriptDataTable {
	uint32_t	field[0x20];

	JassRegister* find(const string& name);
};

struct JassArray {
	void**			vf_table;		// 0x0
	uint32_t		capacity;		// 0x4
	uint32_t		max_size;		// 0x8
	uint32_t*		arr;			// 0xC
	unsigned char	ukn_0x10[0x4];	// 0x10
}; // sizeof = 0x14

struct JassStackStruct {
	JassStackStruct*	next_stack;				// 0x0	下一个栈 双向链表
	JassStackStruct*	last_stack;				// 0x4	上一个栈
	uint32_t			top_0x8;				// 0x8	栈顶
	JassRegister*		frame[0x20];			// 0xC	栈元素
	uint32_t			top_0x8C;				// 0x8C 栈顶
	ScriptDataTable		local_table;			// 0x90 局部变量表
};// 0x8C后是ScriptDataTable

struct JassSymbol {
	uint32_t			string_hash;			// 0x0
	unsigned char		ukn_0x4[0x10];
	const char*			name;					// 0x14
	uint32_t			index;					// 0x18
};

struct JassSymbolArray {
	unsigned char		ukn_0x0[0x8];			// 似乎是两个计数
	JassSymbol**		symbol_array;			// 0x8
};

struct JassSymbolStruct {
	OPCode*				start_line;				// 0x0 jass脚本字节码起始位置
	unsigned char		ukn_0x4[0x4];			// 似乎是脚本长度
	JassSymbolArray*	table;					// 0x8 符号表	符号表 + 0x8->符号数组
};

struct OPCode {
	byte_t				r3;
	byte_t				r2;
	byte_t				r1;
	byte_t				op;
	uint32_t			arg;

	OPCode(byte_t op_, byte_t r1_, byte_t r2_, byte_t r3_, uint32_t arg_) :
		op(op_), r1(r1_), r2(r2_), r3(r3_), arg(arg_)
	{}
};

struct JassFuncStruct {
	uint32_t			string_hash;			// 0x0
	unsigned char		ukn_0x4[0x10];
	const char*			func_name;				// 0x14
	OPCode*				func_addr;				// 0x18
	uint32_t			ret_type;				// 0x1C		0 - nothing, 1 - boolean, 2 - code, 3 - handle, 4 - integer, 5 - real, 6 - string
};// sizeof = 0x20

struct NativeFuncNode {
	void*				func_addr;				// 0x0
	uint32_t			param_count;			// 0x4
	const char*			param_info;				// 0x8
	unsigned char		ukn_0xC[0x10];
	uint32_t			ret_type;				// 0x1C
}; // sizeof = 0x20

struct NativeFuncStruct {
	void**				vf_table;				// 0x0
	uint32_t			string_hash;			// 0x4
	unsigned char		ukn_0x8[0x8];
	uintptr_t			last_node;				// 0x10
	uintptr_t			next_node;				// 0x14
	const char*			func_name;				// 0x18 key
	NativeFuncNode		func;					// 0x1C value
};// sizeof = 0x3C

struct CGxDevice {
	void**				vf_table;					// 0x0
	byte_t				ukn_0x4[0x58];
	uint32_t			total_positions;			// 0x5C
	byte_t				ukn_0x60[0x1C];
	uint32_t			image_pool_capacity;		// 0x7C
	uint32_t			image_pool_size;			// 0x80
	uint32_t*			image_pool;					// 0x84
	byte_t				ukn_0x88[0x30];
	int32_t				blp_size_limit;				// 0xB8
	byte_t				ukn_0xBC[0x4A0];
	BOOL				loading_image;				// 0x55C
	byte_t				ukn_0x560[0x1C];
	HDC					hdc;						// 0x57C
	byte_t				ukn_0x580[0x604];
	uint32_t			buffer_positions_capacity;	// 0xB84
	uint32_t			buffer_positions_size;		// 0xB88
	float*				buffer_positions;			// 0xB8C
	byte_t				ukn_0xB90[0x30];
	uint32_t			draw_ele_mode;				// 0xBC0 (pGameDLL + 0xB66D8C)[draw_ele_mode]存放了glDrawElements的第一个参数
	uint32_t			draw_ele_count;				// 0xBC4 glDrawElements的第二个参数
	uintptr_t			draw_ele_indices;			// 0xBC8 glDrawElements的第四个参数
};

struct BlpHeader {
	uint32_t			magic;						// 0x0
	uint32_t			type;						// 0x4  0: JPEG, 1: palette
	uint32_t			flag;						// 0x8  alpha
	uint32_t			width;						// 0xC
	uint32_t			height;						// 0x10
	uint32_t			alpha_encoding;				// 0x14 3, 4: Alpha list, 5: Alpha from palette
	uint32_t			subtype;					// 0x18
	uint32_t			offset[0x10];				// 0x1C
	uint32_t			size[0x10];					// 0x5C
};

struct GameTimerTable;
struct GameTimerTable {
	unsigned char		ukn_0x0[0x20];
	uint32_t			timer_count;			// 0x20
	unsigned char		ukn_0x24[0x1C];
	float				cur_stamp;				// 0x40
	unsigned char		ukn_0x44[0x4];
	float				duration_limit;			// 0x48
};

struct GameTimer {
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x3C];
	GameTimerTable*		running_timer;			// 0x40
	GameTimerTable*		idle_timer;				// 0x44
	unsigned char		ukn_0x48[0x38];
	float				move_speed_limit;		// 0x80
};

struct CStringRep;

struct CMapSetupWar3 {
	unsigned char		ukn_0x0[0xC];
	CStringRep*			map_name;				// 0xC
	unsigned char		ukn_0x10[0x8];
	CStringRep*			map_description;		// 0x18
	unsigned char		ukn_0x1C[0x10];
	CStringRep*			map_path;				// 0x2C
}; // sizeof = ?

struct CGameWar3 {
	void**				vf_table;				// 0x0
	uint32_t			ukn_4;					// 0x4
	uint32_t			cur_thread;				// 0x8 当前线程id
	GameStack<uint32_t>	stack_0xC;				// 0xC
	byte_t				ukn_0x18[0x4];
	CGameState*			game_state;				// 0x1C
	byte_t				ukn_0x20[0x8];
	uint32_t			local_player;			// 0x28
	byte_t				ukn_0x2C[0x4];
	CMapSetupWar3*		map_setup;				// 0x30
	byte_t				ukn_0x34[0x10];
	uint32_t			max_player;				// 0x44 - Max player slots on the map.
	byte_t				ukn_0x48[0x04];
	uint32_t			active_player;			// 0x4C - Active players.
	byte_t				ukn_0x50[0x08];
	CPlayerWar3*		players[16];			// 0x58
};

// 漂浮文字文本
struct CTextTagText {
	byte_t				ukn_0x0[0x30];
	uint32_t			shadow_color;			// 0x30
	byte_t				ukn_0x34[0x28];
	const char*			text;					// 0x5C
};

// 漂浮文字
// 实际上是一个结构体没有名字
struct CTextTag {
	glm::vec3			position;
	byte_t				ukn_0xC[0x18];
	uint32_t			color;					// 0x24
	uint32_t			alpha;					// 0x28 会同时影响color
	CTextTagText*		text;					// 0x2C
	uint32_t			flag;					// 0x30 0x10表示无效漂浮文字
};	// sizeof = 0x34?

struct CTextTagManager {
	void**				vf_table;				// 0x0
	GameArray<CTextTag>	running;				// 0x4	运行栈
	uint32_t			running_count;			// 0x14 当前存在的漂浮文字数量
	GameArray<uint32_t>	idles;					// 0x18 回收栈
	byte_t				ukn_0x28[0x8];
	void*				font;					// 0x30 字体文件
	byte_t				ukn_0x34[0x54];
}; // sizeof = 0x88

struct CGameState {
	byte_t				ukn_0x0[0x184];
	JassHandleTable		idle_handles;			// 0x184
	JassHandleTable		handle_table;			// 0x194
	byte_t				ukn_0x1A4[0x124];
	CTextTagManager		textag_manager;			// 0x2C8
};



struct CDoodadBuildInfo {
	union										// 0x0
	{
		uint32_t			id;
		const char			id_str[0x4];
	};
	uint32_t			style;					// 0x4 样式
	glm::vec3			position;				// 0x8
	float				facing;					// 0x14 面向角度 弧度制
	glm::vec3			scale;					// 0x18 三轴缩放
};

struct CDoodad {
	uint32_t			index;					// 0x0 在CDoodads数组中的序号
	CDoodadBuildInfo	info;					// 0x4 构造数据
	glm::mat3x3			matrix;					// 0x28
	glm::vec3			position;				// 0x4C
	byte_t				ukn_0x58[0x2C];
	uint32_t			flag_0x84;				// 0x84
	byte_t				ukn_0x88[0xBC];
	CDoodads*			doodads;				// 0x144
	byte_t				ukn_0x148[0x10];
	uint32_t			we_id;					// 0x158 在编辑器中的id
	byte_t				ukn_0x15C[0x2C];
}; // sizeof = 0x188

struct CDoodadDB;
struct CDestructableDB;

struct CDoodads {
private:
	using doodads_t = GameArray2<CDoodad>;
	using renders_t = GameArray2<uint32_t>;
	using cells_t	= GameArray2<renders_t>;
public:
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	byte_t				ukn_0x8[0x4];
	doodads_t			doodads;				// 0xC 全局装饰物表
	byte_t				ukn_0x1C[0x20];
	renders_t			renders;				// 0x3C 当前渲染的装饰物
	cells_t				cells;					// 0x4C 每个格点上的装饰物
	uint32_t			stride_y;				// 0x5C 一列的格点数
	uint32_t			stride_x;				// 0x60 待定
	uint32_t			start_y;				// 0x64
	uint32_t			start_x;				// 0x68
	uint32_t			end_y;					// 0x6C
	uint32_t			end_x;					// 0x70
	byte_t				ukn_0x74[0x80];
	CDoodadDB*			database_doodad;		// 0xF4 装饰物数据库
	CDestructableDB*	database_destructable;	// 0xF8 可破坏物数据库
};

struct CCheatData {
	void**				vf_table;				// 0x0
	uint32_t			cheat_flag;				// 0x4
};

struct GameTable {
	CCheatData*			cheat_data;				// 0x0

	// wip
};

struct CPoReal {	
// real_value = base_value + time_factor * (widget_timer->cur_stamp - last_stamp + 300 * (widget_timer->cur_cycle - last_cycle))
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x6C];
	float				last_stamp;				// 0x70
	float				last_cycle;				// 0x74
	float				base_value;				// 0x78
	float				time_factor;			// 0x7C
	float				min_value;				// 0x80
	float				max_value;				// 0x84
};

struct CDataStore {
	void**				vf_table;				// 0x0
	const void*			packet;					// 0x4
	uint32_t			ukn_0x8;
	uint32_t			ukn_0xC;
	uint32_t			size;					// 0x10
	int32_t				recv_bytes;				// 0x14	default -1

	CDataStore(void** _vf_table, const void* _packet, uint32_t _size)
		: vf_table(_vf_table)
		, packet(_packet)
		, ukn_0x8(0)
		, ukn_0xC(0x5B4)
		, size(_size)
		, recv_bytes(-1)
	{}
};

struct AnimSequenceInfo {
	int32_t				internal_sequence_id;	// 0x0
	const char*			name;					// 0x4 指向 AnimationSequenceData+0x3C
	uint32_t			duration_ms;			// 0x8 interval_end - interval_start
	float				move_speed;				// 0xC
	uint32_t			priority;				// 0x10
	float				rarity;					// 0x14
	uint32_t			flags;					// 0x18 bit0=循环等
	int32_t				sub_animation_index;	// 0x1C
}; // sizeof = 0x20

struct AnimationSequenceData {
	uint32_t			interval_start_ms;		// 0x0
	uint32_t			interval_end_ms;		// 0x4
	float				move_speed;				// 0x8
	uint32_t			flags;					// 0xC
	uint32_t			priority;				// 0x10
	float				rarity;					// 0x14
	byte_t				ukn_0x18[0x24];
	char				name[0x50];				// 0x3C 模型内联序列名
}; // sizeof = 0x8C

struct AnimationSequenceLookupEntry {
	byte_t				ukn_0x0[0x8];
	uint8_t				internal_sequence_id;	// 0x8 0xFF=无效
	byte_t				ukn_0x9[0x3];
}; // sizeof = 0xC

struct CAnimSequenceProvider {
	byte_t								ukn_0x0[0x8];
	AnimationSequenceLookupEntry*		external_to_internal_map;	// 0x8
	uint32_t							external_sequence_count;	// 0xC
	byte_t								ukn_0x10[0x8];
	AnimationSequenceData*				sequence_data;				// 0x18
	uint32_t							sequence_count;				// 0x1C
}; // sizeof = 0x20

struct AnimationTimeState {
	uint32_t			stamp;					// 0x0 当前时间戳/毫秒
	uint32_t			flags;					// 0x4 活跃计数/混合状态
	byte_t				ukn_0x8[0x8];
}; // sizeof = 0x10

struct CAnimComplex {
	byte_t						ukn_0x0[0x8];
	AnimationTimeState*			time_states;				// 0x8 每序列 0x10 字节
	uint32_t					sequence_count;				// 0xC
	byte_t						ukn_0x10[0x34];
	CAnimSequenceProvider*		sequence_provider;			// 0x44
	float						time_scale;					// 0x48
	byte_t						ukn_0x4C[0x8];
	uint32_t					flag_0x54;					// 0x54
	uint8_t						current_sequence_index;		// 0x58
	byte_t						ukn_0x59[0x17];
	uint32_t*					bone_blend_buffer;			// 0x70
	uint32_t					bone_blend_count;			// 0x74
	byte_t						ukn_0x78[0x6C];
	uint32_t					blend_transition_time;		// 0xE4
}; // sizeof = 0xE8

struct CModelComplex {
	void**				vf_table;				// 0x0
	byte_t				ukn_0x4[0x94];
	CAnimComplex*		animation;				// 0x98
};

// CWidget 公共前缀：单位/物品/可破坏物等都共用这段头布局。
struct CWidget {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	uint32_t			ukn_0x8;				// 0x8
	uint32_t			hash_0xC;				// 0xC
	uint32_t			hash_0x10;				// 0x10
	unsigned char		ukn_0x14[0xC];
	uint32_t			flag_0x20;				// 0x20
	unsigned char		ukn_0x24[0x4];
	CSprite*			sprite;					// 0x28
	unsigned char		ukn_0x2C[0x4];
	uint32_t			type_id;				// 0x30 单位/物品等对象的 rawcode 或类型ID
}; // sizeof = 0x34

// CSprite 的动画请求负载：在不同调用路径里可承载 sequence id 数组或时间值数组。
struct CSpriteAnimPayloadArray {
	uint32_t			capacity;				// 0x0
	uint32_t			size;					// 0x4
	void*				data;					// 0x8
	uint32_t			growth_alignment;		// 0xC
}; // sizeof = 0x10

// 环形动画队列里的单个请求（28字节）。
struct CSpriteAnimRequest {
	uint32_t			request_flags;			// 0x0 仅确认高位控制位来自 flags & 0xFC0
	int32_t				request_key_or_sentinel;// 0x4 标签路径存首个 tag；索引路径固定写 -2
	CSpriteAnimPayloadArray	candidate_sequences;	// 0x8 候选 sequence id 动态数组
	uint32_t			inline_sequence_slot1;	// 0x18 小数组路径的第 2 个 inline 槽
}; // sizeof = 0x1C

// `sub_6F12F610 / sub_6F12F630 / sub_6F12FA70` 共同使用的动画控制器前缀。
struct CModelAnimController {
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x94];
	void*				anim_complex;			// 0x98 CAnimComplex*，为空时动画序列接口直接返回失败
}; // sizeof = 0x9C

// 精灵基础类：动画环形队列、挂点附着关系都位于这段前缀。
struct CSprite {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	unsigned char		ukn_0x8[0x18];
	CModelAnimController*	model_runtime;		// 0x20 prerender / set_animation_by_index / 挂点链都会经过这里
	uint32_t			ukn_0x24;				// 0x24 仍待继续确认
	uint32_t			sprite_flags;			// 0x28
	int32_t				cached_sequence_id;		// 0x2C 动画切换后会被置为 -2 强制刷新
	void*				sequence_provider;		// 0x30 与动画序列表/解析器关联
	uint8_t				anim_queue_count;		// 0x34
	uint8_t				anim_queue_head;		// 0x35 环形队列头（旧条目）
	uint8_t				anim_queue_tail;		// 0x36 当前/最新条目
	uint8_t				ukn_0x37;				// 0x37
	uint32_t			anim_queue_storage_hint;// 0x38 resize/growth 相关参数，语义待补
	uint32_t			anim_queue_capacity;	// 0x3C
	CSpriteAnimRequest*	anim_queue_entries;		// 0x40 stride = 0x1C
}; // sizeof = 0x44

struct CSpriteUber : public CSprite {
	unsigned char		ukn_0x44[0x50];
	uint32_t			anim_time_override_enabled;	// 0x94 为 0 时走正常时间推进
	unsigned char		ukn_0x98[0x8];
	float				anim_time_override_sec;	// 0xA0 非零路径会直接写入动画时间
	unsigned char		ukn_0xA4[0x1C];
	float				x;						// 0xC0
	float				y;						// 0xC4
	float				z;						// 0xC8
	unsigned char		ukn_0xCC[0x1C];
	float				uniform_scale;			// 0xE8
	unsigned char		ukn_0xEC[0x1C];
	float				world_matrix_3x4[12];	// 0x108 sub_6F137170 / sub_6F12F0A0 会直接写入 3x4 世界矩阵
	uint32_t			render_flags_0x138;		// 0x138
	unsigned char		ukn_0x13C[0xC];
	uint32_t			color_rgba;				// 0x148
};

// 被挂到 sprite 挂点上的模型实例前缀。
struct CModelInstance {
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x1C];
	void*				renderable;				// 0x20 为空时不会进入附着链
	void*				parent_bone_system;		// 0x24 Detach 时会回到这里
	uint32_t			instance_flags;			// 0x28 0x10000 表示已挂到父 sprite
	unsigned char		ukn_0x2C[0x2];
	int16_t				attached_point_index;	// 0x2E
}; // sizeof = 0x30





struct CDetectedData {
	void*				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	unsigned char		ukn_0x8[0xC];
	CObserver			observer;				// 0x14
	unsigned char		ukn_0x20[0x4];
	uint32_t			visible_flag_1;			// 0x20 1 << player_id
	unsigned char		ukn_0x28[0x4];
	uint32_t			visible_count_1[16];	// 0x2C 计数器
	uint32_t			visible_flag_2;			// 0x6C 1 << player_id
	unsigned char		ukn_0x70[0x4];
	uint32_t			visible_count_2[16];	// 0x74 计数器

};


struct CUnit {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	uint32_t			ukn_0x8;
	uint32_t			hash_0xC;				// 0xC
	uint32_t			hash_0x10;				// 0x10
	unsigned char		ukn_0x14[0xC];
	uint32_t			flag_0x20;				// 0x20
	unsigned char		ukn_0x24[0x4];
	CSprite*			sprite;					// 0x28
	unsigned char		ukn_0x2C[0x4];
	uint32_t			id;						// 0x30
	unsigned char		ukn_0x34[0x1C];
	CPreselectUI*		preselect_ui;			// 0x50
	uint32_t			stun_count;				// 0x54 眩晕计数器
	uint32_t			owner_id;				// 0x58
	uint32_t			flag_0x5C;				// 0x5C
	uint32_t			flag_0x60;				// 0x60 0x1: 小动物; 0x2: 龙卷风漫步者; 0x20 可选择?
	unsigned char		ukn_0x64[0x3C];
	uint32_t			hash_0xA0;				// 0xA0
	uint32_t			hash_0xA4;				// 0xA4
	unsigned char		ukn_0xA8[0x18];
	uint32_t			hash_0xC0;				// 0xC0
	uint32_t			hash_0xC4;				// 0xC4
	unsigned char		ukn_0xC8[0x18];
	float				def_value;				// 0xE0
	uint32_t			def_type;				// 0xE4
	unsigned char		ukn_0xE8[0x14];
	FloatStruct			cur_sight;				// 0xFC
	uint32_t			vision_level;			// 0x10C
	unsigned char		ukn_0x110[0x4];
	uint32_t			invision_count;			// 0x114 隐形计数器
	FloatStruct			float_0x118;			// 0x118
	unsigned char		ukn_0x128[0x28];
	uint32_t			freeze_count;			// 0x150 冰冻计数器
	uint32_t			animate_count;			// 0x154 操纵死尸计数器
	uint32_t			ethereal_count;			// 0x158
	uint32_t			spell_immune_count;		// 0x15C
	uint32_t			spell_immune_flag;		// 0x160
	unsigned char		ukn_0x164[0x8];
	uint32_t			hash_0x16C;				// 0x16C
	uint32_t			hash_0x170;				// 0x170
	HashGroup			task_hash;				// 0x174
	unsigned char		ukn_0x17C[0x10];
	uint32_t			flag_0x18C;				// 0x18C
	uint32_t			ukn_0x190;
	uint32_t			state;					// 0x194
	uint32_t			stun_counter;			// 0x198
	uint32_t			cur_order_hash1;		// 0x19C
	uint32_t			cur_order_hash2;		// 0x1A0
	uint32_t			ukn_0x1A4;
	HashGroup			last_order;				// 0x1A8
	uint32_t			ukn_0x1B0;
	uint32_t			order_count;			// 0x1B4
	unsigned char		ukn_0x1B8[0x18];
	uint32_t			silence_count;			// 0x1D0
	unsigned char		ukn_0x1D4[0x8];
	HashGroup			abil_list_hash;			// 0x1DC
	uint32_t			ukn_0x1E4;
	CAbilityAttack*		ability_attack;			// 0x1E8
	CAbilityMove*		ability_move;			// 0x1EC
	CAbilityHero*		ability_hero;			// 0x1F0
	CAbilityBuild*		ability_build;			// 0x1F4
	CAbilityInventory*	ability_inventory;		// 0x1F8
	uint32_t			flag_0x1FC;				// 0x1FC
	unsigned char		ukn_0x200[0x8];
	float				fly_height;				// 0x208
	unsigned char		ukn_0x20C[0x1C];
	float				impact_z;				// 0x228
	float				impact_z_swim;			// 0x22C
	float				launch_x;				// 0x230
	float				launch_y;				// 0x234
	float				launch_z;				// 0x238
	float				launch_z_swim;			// 0x23C
	unsigned char		ukn_0x240[0x8];
	uint32_t			flag_0x248;				// 0x248 0x2
	uint32_t			as_target_type;			// 0x24C 作为目标类型
	HashGroup			killer;					// 0x250 凶手哈希
	unsigned char		ukn_0x258[0x28];
	uint32_t			flag_0x280;				// 0x280 0x200表示显示小地图特殊图标 0x400表示不可选中
	unsigned char		ukn_0x284[0x30];
	float				z_scale;				// 0x2B4
	unsigned char		ukn_0x2B8[0x14];
	float				hpbar_height;			// 0x2CC
	unsigned char		ukn_0x2D0[0x30];
	float				time_scale;				// 0x300
	unsigned char		ukn_0x304[0x8];
}; //sizeof = 0x30C

struct UnitDefData {
	uint32_t						hash_key;					// 0x0
	unsigned char					ukn_0x4[0x10];
	uint32_t						id;							// 0x14
	uint32_t						build_time;					// 0x18
	uint32_t						repair_time;				// 0x1C
	uint32_t						gold_cost;					// 0x20
	uint32_t						lumber_cost;				// 0x24
	uint32_t						gold_repair;				// 0x28
	uint32_t						lumber_repair;				// 0x2C
	uint32_t						gold_bounty_dice;			// 0x30
	uint32_t						gold_bounty_sides;			// 0x34
	uint32_t						gold_bounty_base;			// 0x38
	uint32_t						lumber_bounty_dice;			// 0x3C
	uint32_t						lumber_bounty_sides;		// 0x40
	uint32_t						lumber_bounty_base;			// 0x44
	uint32_t						stock_max;					// 0x48
	float							stock_regen;				// 0x4C
	float							stock_start;				// 0x50
	uint32_t						can_sleep;					// 0x54
	uint32_t						can_flee;					// 0x58
	uint32_t						food_used;					// 0x5C
	uint32_t						food_made;					// 0x60
	uint32_t						cargo_size;					// 0x64
	uint32_t						level;						// 0x68
	float							cast_point;					// 0x6C
	float							cast_backswing;				// 0x70
	float							death_time;					// 0x74
	uint32_t						regen_type;					// 0x78
	float							life_regen;					// 0x7C
	float							max_life;					// 0x80
	float							init_mana;					// 0x84
	float							max_mana;					// 0x88
	float							mana_regen;					// 0x8C
	float							def_value;					// 0x90
	float							def_up;						// 0x94
	uint32_t						def_type;					// 0x98
	uint32_t						weapons_on;					// 0x9C
	uint32_t						target_allow1;				// 0xA0
	uint32_t						target_allow2;				// 0xA4
	uint32_t						damage_up1;					// 0xA8
	uint32_t						damage_up2;					// 0xAC
	uint32_t						damage_dice1;				// 0xB0
	uint32_t						damage_dice2;				// 0xB4
	uint32_t						damage_sides1;				// 0xB8
	uint32_t						damage_sides2;				// 0xBC
	uint32_t						damage_base1;				// 0xC0
	uint32_t						damage_base2;				// 0xC4
	uint32_t						target_count1;				// 0xC8
	uint32_t						target_count2;				// 0xCC
	float							damage_loss1;				// 0xD0
	float							damage_loss2;				// 0xD4
	float							spill_dist1;				// 0xD8
	float							spill_dist2;				// 0xDC
	float							spill_radius1;				// 0xE0
	float							spill_radius2;				// 0xE4
	uint32_t						attack_type1;				// 0xE8
	uint32_t						attack_type2;				// 0xEC
	uint32_t						weapon_sound1;				// 0xF0
	uint32_t						weapon_sound2;				// 0xF4
	uint32_t						weapon_type1;				// 0xF8
	uint32_t						weapon_type2;				// 0xFC
	float							attack_range1;				// 0x100
	float							attack_range2;				// 0x104
	float							range_buffer1;				// 0x108
	float							range_buffer2;				// 0x10C
	float							bat1;						// 0x110
	float							bat2;						// 0x114
	float							attack_point1;				// 0x118
	float							attack_point2;				// 0x11C
	float							attack_backswing1;			// 0x120
	float							attack_backswing2;			// 0x124
	unsigned char					ukn_0x128[0x8];
	float							full_area1;					// 0x130
	float							full_area2;					// 0x134
	float							half_area1;					// 0x138
	float							half_area2;					// 0x13C
	float							small_area1;				// 0x140
	float							small_area2;				// 0x144
	float							half_factor1;				// 0x148
	float							half_factor2;				// 0x14C
	float							small_factor1;				// 0x150
	float							small_factor2;				// 0x154
	unsigned char					ukn_0x158[0x10];
	uint32_t						show_UI1;					// 0x168
	uint32_t						show_UI2;					// 0x16C
	uint32_t						init_str;					// 0x170
	uint32_t						init_agi;					// 0x174
	uint32_t						init_int;					// 0x178
	uint32_t						primary_attr;				// 0x17C
	float							str_up;						// 0x180e
	float							agi_up;						// 0x184
	float							int_up;						// 0x188
	float							sight_day;					// 0x18C
	float							sight_night;				// 0x190
	float							acquision_range;			// 0x194
	float							min_range;					// 0x198
	float							collision;					// 0x19C
	float							fog_radius;					// 0x1A0
	unsigned char					ukn_0x1A4[0x8];
	uint32_t						collision_type_from_other;	// 0x1AC
	uint32_t						collision_type_to_other;	// 0x1B0
	unsigned char					ukn_0x1B4[0x8];
	float							AI_radius;					// 0x1BC
	uint32_t						race;						// 0x1C0
	unsigned char					ukn_0x1C4[0x4];
	uint32_t						type;						// 0x1C8
	unsigned char					ukn_0x1CC[0x4];
	uint32_t						points;						// 0x1D0
	float							speed;						// 0x1D4
	float							min_speed;					// 0x1D8
	float							max_speed;					// 0x1DC
	float							turn_rate;					// 0x1E0
	float							prop_win;					// 0x1E4 转向角度
	float							orient_interp;				// 0x1E8 转向补正
	unsigned char					ukn_0x1EC[0x4];
	float							occlusion_height;			// 0x1F0
	unsigned char					ukn_0x1F4[0x4];
	float							height;						// 0x1F8
	float							move_floor;					// 0x1FC
	float							launch_X;					// 0x200
	float							launch_Y;					// 0x204
	float							launch_Z;					// 0x208
	float							launch_Z_swim;				// 0x20C
	float							impact_Z;					// 0x210
	float							impact_Z_swim;				// 0x214
	float							blend;						// 0x218
	float							walk_speed;					// 0x21C
	float							run_speed;					// 0x220
	byte_t							ukn_0x224[0x4];
	uint32_t						repluse_param;				// 0x228 组群分离 - 参数
	uint32_t						repluse_group;				// 0x22C 组群分离 - 租号
	uint32_t						repluse_priority;			// 0x230 组群分离 - 优先权
	GameSimpleArray<uint32_t>		upgrades;					// 0x234 使用科技
	byte_t							ukn_0x240[0xC];
	uint32_t						atk_upgrade;				// 0x24C 似乎是攻击科技
	uint32_t						def_upgrade;				// 0x250 似乎是防御科技
	byte_t							ukn_0x254[0x1C];
	uint32_t						auto_ability;				// 0x270 默认主动技能
	RCString						abil_list;					// 0x274 技能列表
	RCString						ukn_0x280;					// 未知字符串
	byte_t							ukn_0x28C[0x10];
	RCString						ukn_0x29C;					// 未知字符串
	RCString						ukn_0x2A8;					// 未知字符串 可能是单位名或者语音?
	byte_t							ukn_0x2B4[0x8];
}; // sizeof = 0x2BC

struct UnitUIDefData {
	void**							vf_table;				// 0x0
	uint32_t						hash_key;				// 0x4
	unsigned char					ukn_0x8[0x10];
	uint32_t						id;						// 0x18
	uint32_t						reverse_id;				// 0x1C
	unsigned char					ukn_0x20[0xC];
	const char**					name;					// 0x2C
	unsigned char					ukn_0x30[0x4];
	const char*						model;					// 0x34
	const char*						portrait;				// 0x38
	GameSimpleArray<const char*>	missile_art;			// 0x3C
	unsigned char					ukn_0x48[0xC];
	float							circle_scale;			// 0x54
	float							circle_Z;				// 0x58
	unsigned char					ukn_0x5C[0x8];
	uintptr_t						missile_speed;			// 0x64
	unsigned char					ukn_0x68[0x8];
	const char**					missile_arc;			// 0x70
	unsigned char					ukn_0x74[0xC];
	float							shadow_X;				// 0x80
	float							shadow_Y;				// 0x84
	float							shadow_width;			// 0x88
	float							shadow_height;			// 0x8C
	unsigned char					ukn_0x90[0x1C];
	uint32_t						color;					// 0xAC
	float							scale;					// 0xB0
	unsigned char					ukn_0xB4[0x24];
	GameSimpleArray<const char*>	awaken_tips;			// 0xD8 提示工具 - 唤醒
	GameSimpleArray<const char*>	revive_tips;			// 0xE4 提示工具 - 重生
	GameSimpleArray<const char*>	proper_names;			// 0xF0
	GameSimpleArray<uint32_t>		anim_props;				// 0xFC
	GameSimpleArray<uint32_t>		attach_props;			// 0x108
	GameSimpleArray<uint32_t>		attach_link_props;		// 0x114
	GameSimpleArray<uint32_t>		bone_props;				// 0x120
	const char*						score_screen_icon;		// 0x12C
	const char*						movement;				// 0x130 ?似乎只有骑马才有 tbd
	unsigned char					ukn_0x134[0x4];
	uint32_t						looping_snd_fade_in;	// 0x138
	uint32_t						looping_snd_fade_out;	// 0x13C
	unsigned char					ukn_0x140[0x28];
	GameSimpleArray<uint32_t>		dependency_or;			// 0x168 从属等价物
	GameSimpleArray<uint32_t>		ukn_0x174;				// 未知数组
	GameSimpleArray<uint32_t>		upgrades;				// 0x180 建筑升级
	GameSimpleArray<uint32_t>		ukn_0x18C;				// 未知数组
	GameSimpleArray<uint32_t>		builds;					// 0x198 可建造建筑物
	GameSimpleArray<uint32_t>		ukn_0x1A4;				// 未知数组
	GameSimpleArray<uint32_t>		trains;					// 0x1B0 训练单位
	GameSimpleArray<uint32_t>		ukn_0x1BC;				// 未知数组
	GameSimpleArray<uint32_t>		researches;				// 0x1C8 可研究项目
	GameSimpleArray<uint32_t>		ukn_0x1D4;				// 未知数组
	GameSimpleArray<uint32_t>		sell_units;				// 0x1E0 售出单位
	GameSimpleArray<uint32_t>		ukn_0x1EC;				// 未知数组
	GameSimpleArray<uint32_t>		sell_items;				// 0x1F8 售出物品
	GameSimpleArray<uint32_t>		ukn_0x204;				// 未知数组
	GameSimpleArray<uint32_t>		make_items;				// 0x210 制造物品
	byte_t							ukn_0x21C[0x30];
	const char**					art;					// 0x24C
	uint32_t						button_X;				// 0x250
	uint32_t						button_Y;				// 0x254
	unsigned char					ukn_0x258[0x8];
	const char**					tip;					// 0x260
	unsigned char					ukn_0x264[0x8];
	const char**					ubertip;				// 0x26C
	unsigned char					ukn_0x270[0x8];
	uint32_t*						hotkey;					// 0x278
	byte_t							ukn_0x27C[0x24];
	GameSimpleArray<uint32_t>		require;				// 0x2A0 科技需求
	GameSimpleArray<uint32_t>		ukn_0x2AC;				// 未知数组
	GameSimpleArray<uint32_t>		require_amout;			// 0x2B8 科技需求值
};

struct CPreselectUI {
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x4];			// 0x4
	CUnit*				owner;					// 0x8
	CStatBar*			hp_bar;					// 0xC
}; // sizeof = 0x10



struct AbilityCastData {	// 构造非无目标技能时


}; // sizeof = 0x18


struct CAbility {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	uint32_t			flag_0x8;				// 0x8
	uint32_t			hash_0xC;				// 0xC
	uint32_t			hash_0x10;				// 0x10
	unsigned char		ukn_0x14[0xC];
	uint32_t			flag_0x20;				// 0x20
	HashGroup			next_abil_hash;			// 0x24
	uint32_t			is_buff;				// 0x2C buff这里是1
	CUnit*				owner;					// 0x30
	uint32_t			id;						// 0x34
	uint32_t			permanent_count;		// 0x38 永久性计数器
	uint32_t			disable_count;			// 0x3C	禁用计数器
	uint32_t			hide_count;				// 0x40	隐藏计数器
	uint32_t			disable_ex_count;		// 0x44	禁用Ex计数器
	unsigned char		ukn_0x48[0x4];
	uint32_t			target_allow;			// 0x4C 当前目标允许
	uint32_t			level;					// 0x50 等级-1
	AbilityDefData*		def_data;				// 0x54
	CAgentTimer*		timer_0x58;				// 0x58
	unsigned char		ukn_0x5C[0x10];
}; // sizeof = 0x6C

struct CAbilityAttack {
	CAbility			base_ability;
	HashGroup			cur_target_hash;		// 0x6C
	unsigned char		ukn_0x74[0x4];
	HashGroup			last_target_hash;		// 0x78
	unsigned char		ukn_0x80[0x8];
	uint32_t			damage_dice1;			// 0x88
	uint32_t			damage_dice2;			// 0x8C
	unsigned char		ukn_0x90[0x4];
	uint32_t			damage_sides1;			// 0x94
	uint32_t			damage_sides2;			// 0x98
	unsigned char		ukn_0x9C[0x4];
	uint32_t			base_damage1;			// 0xA0
	uint32_t			base_damage2;			// 0xA4
	unsigned char		ukn_0xA8[0x4];
	uint32_t			bonus_damage1;			// 0xAC
	uint32_t			bonus_damage2;			// 0xB0
	unsigned char		ukn_0xB4[0x28];
	uint32_t			weapon_type1;			// 0xDC
	uint32_t			weapon_type2;			// 0xE0
	unsigned char		ukn_0xE4[0x4];
	uint32_t			weapon_sound;			// 0xE8
	unsigned char		ukn_0xEC[0x8];
	uint32_t			attack_type1;			// 0xF4
	uint32_t			attack_type2;			// 0xF8
	unsigned char		ukn_0xFC[0x4];
	uint32_t			target_count1;			// 0x100
	uint32_t			target_count2;			// 0x104
	unsigned char		ukn_0x108[0x50];
	float				bat1;					// 0x158
	unsigned char		ukn_0x15C[0x4];
	float				bat2;					// 0x160
	unsigned char		ukn_0x164[0x8];
	float				attack_point1;			// 0x16C
	unsigned char		ukn_0x170[0xC];
	float				attack_point2;			// 0x17C
	unsigned char		ukn_0x180[0x10];
	float				back_swing1;			// 0x190
	unsigned char		ukn_0x194[0xC];
	float				back_swing2;			// 0x1A0
	unsigned char		ukn_0x1A4[0xC];
	float				attack_speed;			// 0x1B0
	unsigned char		ukn_0x1B4[0x24];
	CAgentTimer			attack_timer;			// 0x1D8
	CAgentTimer			attackpoint_timer;		// 0x1EC
	CAgentTimer			backswing_timer;		// 0x200
	unsigned char		ukn_0x214[0x4];
	uint32_t			target_allow1;			// 0x218 
	uint32_t			target_allow2;			// 0x21C
	uint32_t			disable_auto;			// 0x220 禁用自动攻击
	unsigned char		ukn_0x224[0x14];
	CSplashData*		splash_data1;			// 0x238
	CSplashData*		splash_data2;			// 0x23C
	unsigned char		ukn_0x240[0x4];
	float				acquision_range;		// 0x244
	unsigned char		ukn_0x248[0x4];
	float				min_range;				// 0x24C
	unsigned char		ukn_0x250[0x8];
	float				attack_range1;			// 0x258
	unsigned char		ukn_0x25C[0x4];
	float				attack_range2;			// 0x260
	unsigned char		ukn_0x264[0x8];
	float				range_buffer1;			// 0x26C
	unsigned char		ukn_0x270[0x4];
	float				range_buffer2;			// 0x274
	unsigned char		ukn_0x278[0x34];
	float				cur_target_x;			// 0x2AC
	unsigned char		ukn_0x2B0[0x4];
	float				cur_target_y;			// 0x2B4
	uint32_t			cur_weapons_on;			// 0x2B8
};

struct CAbilityHero {
	CAbility			base_ability;
	Integer				hero_level;				// 0x6C level - 1
	Integer				base_int;				// 0x7C init_int + bonus_int
	uint32_t			cur_exp;				// 0x8C
	uint32_t			skill_point;			// 0x90
	uint32_t			base_str;				// 0x94 init_str + bonus_str
	unsigned char		ukn_0x98[0x4];
	uint32_t			str;					// 0x9C
	unsigned char		ukn_0xA0[0x4];
	float				str_bonus_hp;			// 0xA4
	uint32_t			base_agi;				// 0xA8 init_agi + bonus_agi
	unsigned char		ukn_0xAC[0x4];
	float				agi_bonus_def;			// 0xB0
	unsigned char		ukn_0xB4[0xC];
	float				agi_bonus_atkspd;		// 0xC0
	unsigned char		ukn_0xC4[0x4];
	float				int_bonus_mp;			// 0xC8
	uint32_t			primary;				// 0xCC (1 = str; 2 = int; 3 = agi)
	unsigned char		ukn_0xD0[0x4];
	float				str_per_level;			// 0xD4
	unsigned char		ukn_0xD8[0x4];
	float				int_per_level;			// 0xDC
	unsigned char		ukn_0xE0[0x4];
	float				agi_per_level;			// 0xE4
	uint32_t			flag_0xE8;				// 0xE8 &0xFF表示称谓
	uint32_t			disable_exp;			// 0xEC 禁止获取经验值
	unsigned char		ukn_0xF0[0x4];
	uint32_t			abil_list[5];			// 0xF4
	unsigned char		ukn_0x108[0x4];
	uint32_t			max_level_list[5];		// 0x10C
	unsigned char		ukn_0x120[0x4];
	uint32_t			req_level_list[5];		// 0x124
	unsigned char		ukn_0x128[0x1C];
};	// sizeof = 0x140

struct CAbilityInventory {
	CAbility			base_ability;
	uint32_t			max_slot;				// 0x6C
	HashCargo			item_hash[0x6];			// 0x70	物品哈希
};

struct AbilityDefData {
	uint32_t			hash_key;				// 0x0
	unsigned char		ukn_0x4[0x24];
	AbilityUIDefData*	ui_def_data;			// 0x28
	uint32_t			is_ability;				// 0x2C ?待验证
	uint32_t			base_id;				// 0x30
	uint32_t			id;						// 0x34
	uint32_t			is_hero;				// 0x38	?待验证
	uint32_t			max_level;				// 0x3C
	uint32_t			req_level;				// 0x40
	uint32_t			level_skip;				// 0x44
	int32_t				priority;				// 0x48
	unsigned char		ukn_0x4C[0x8];
	AbilityLevelData*	level_data;				// 0x54
};

struct AbilityLevelData {
	uint32_t			target_allow;			// 0x0
	float				cast_time;				// 0x4
	float				dur_normal;				// 0x8
	float				dur_hero;				// 0xC
	uint32_t			mana_cost;				// 0x10
	float				cooldown;				// 0x14
	float				area;					// 0x18
	float				range;					// 0x1C
	float				data_A;					// 0x20
	float				data_B;					// 0x24
	float				data_C;					// 0x28
	float				data_D;					// 0x2C
	float				data_E;					// 0x30
	float				data_F;					// 0x34
	float				data_G;					// 0x38
	float				data_H;					// 0x3C
	float				data_I;					// 0x40
	uint32_t			unit_id;				// 0x44 召唤单位id
	unsigned char		ukn_0x48[0x20];
};

struct AbilityUIDefData {
	void**							vf_table;				// 0x0
	unsigned char					ukn_0x4[0x14];
	uint32_t						id;						// 0x18
	uint32_t						reverse_id;				// 0x1C
	unsigned char					ukn_0x20[0x8];
	char*							buff_art;				// 0x28
	unsigned char					ukn_0x2C[0x4];
	const char*						effect_sound;			// 0x30
	const char*						effect_sound_looped;	// 0x34
	const char*						unart;					// 0x38
	const char*						research_art;			// 0x3C
	const char*						selector_model;			// 0x40
	const char*						selector_texture;		// 0x44
	const char*						global_message;			// 0x48
	const char*						global_sound;			// 0x4C
	int32_t							button_x;				// 0x50
	int32_t							button_y;				// 0x54
	int32_t							unbutton_x;				// 0x58
	int32_t							unbutton_y;				// 0x5C
	int32_t							research_button_x;		// 0x60
	int32_t							research_button_y;		// 0x64
	float							missile_speed;			// 0x68
	float							missile_arc;			// 0x6C
	uint32_t						missile_homing;			// 0x70
	unsigned char					ukn_0x74[0x10];
	uint32_t*						hotkey;					// 0x84
	unsigned char					ukn_0x88[0x8];
	uint32_t*						unhotkey;				// 0x90
	unsigned char					ukn_0x94[0x8];
	uint32_t*						research_hotkey;		// 0x9C
	GameSimpleArray<const char*>	name;					// 0xA0
	GameSimpleArray<const char*>	art;					// 0xAC
	GameSimpleArray<const char*>	target_art;				// 0xB8
	GameSimpleArray<const char*>	caster_art;				// 0xC4
	GameSimpleArray<const char*>	effect_art;				// 0xD0
	GameSimpleArray<const char*>	area_effect_art;		// 0xDC
	GameSimpleArray<const char*>	missile_art;			// 0xE8
	GameSimpleArray<const char*>	special_art;			// 0xF4
	GameSimpleArray<const char*>	lightning_effect;		// 0x100
	GameSimpleArray<const char*>	buff_tip;				// 0x10C
	GameSimpleArray<const char*>	buff_ubertip;			// 0x118
	GameSimpleArray<const char*>	research_tip;			// 0x124
	GameSimpleArray<const char*>	tip;					// 0x130
	GameSimpleArray<const char*>	untip;					// 0x13C
	GameSimpleArray<const char*>	research_ubertip;		// 0x148
	GameSimpleArray<const char*>	ubertip;				// 0x154
	GameSimpleArray<const char*>	unubertip;				// 0x160
	unsigned char					ukn_0x16C[0x4];
	uint32_t						caster_attach_exist;	// 0x170
	uint32_t						caster_attach_count;	// 0x174
	uintptr_t						caster_attach;			// 0x178
	unsigned char					ukn_0x17C[0x2C];
	uint32_t						target_attach_exist;	// 0x1A8
	uint32_t						target_attach_count;	// 0x1AC
	uintptr_t						target_attach;			// 0x1B0
	unsigned char					ukn_0x1B4[0x90];
}; // sizeof = 0x244

struct CAlliances {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	unsigned char		ukn_0x8[0x4];
	HashGroup			hash_0xC;				// 0xC
	CObserver			observer;				// 0x14
	unsigned char		ukn_0x20[0x4];
	Integer				integer_0x24;			// 0x24
	uint32_t			count;					// 0x34
	Integer				alliance[0x100];		// 0x38
};

struct CPlayerWar3 {
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x2C];
	uint32_t			id;						// 0x30
	CSelectionWar3*		selection;				// 0x34
	CAlliances*			alliance;				// 0x38
	uint32_t			state_count;			// 0x3C 固定为26 (0x19 + 1)
	union PlayerStateUnion {
		Integer			states[0x1A];

		struct PlayerStateFields {
			Integer				score;					// 0x40 游戏得分
			Integer				gold;					// 0x50 当前黄金
			Integer				lumber;					// 0x60 当前木材
			Integer				hero_tokens;			// 0x70 剩余可用英雄数
			Integer				food_cap;				// 0x80 可用人口数
			Integer				food_used;				// 0x90 已用人口数
			Integer				food_ceiling;			// 0xA0 已用人口数
			Integer				gives_bounty;			// 0xB0 给予奖励
			Integer				allied_victory;			// 0xC0 联盟胜利
			Integer				placed;					// 0xD0 放置
			Integer				observer_on_death;		// 0xE0 战败后成为观战者
			Integer				observer;				// 0xF0 裁判或观战者
			Integer				unfollowable;			// 0x100 不可跟随
			Integer				gold_keep_rate;			// 0x110 黄金维修费率
			Integer				lumber_keep_rate;		// 0x120 木材维修费率
			Integer				gold_gathered;			// 0x130 黄金采集量
			Integer				lumber_gathered;		// 0x140 黄金采集量
			Integer				ukn_integers[8];		// 0x150 未知的state	
			Integer				no_creep_sleep;			// 0x1D0 中立敌对玩家单位睡眠
		} fields;
	} player_state;
	unsigned char		ukn_0x1E0[0x68];
	uint32_t			max_food;				// 0x248
	uint32_t			acc_max_food;			// 0x24C
	byte_t				ukn_0x250[0x4];
	uint32_t			acc_pickup_items;		// 0x254
	byte_t				ukn_0x258[0x8];
	uint32_t			race;					// 0x260
	uint32_t			color;					// 0x264
	uint32_t			controller;				// 0x268
	unsigned char		ukn_0x26C[0x4];
	uint32_t			state;					// 0x270
	unsigned char		ukn_0x274[0x4];
	uint32_t			team;					// 0x278
	unsigned char		ukn_0x27C[0x54];
	unsigned char*		techtree;				// 0x2D0
};

struct CSelectionWar3 {
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x10];
	CUnitSet			formation[0xA];			// 0x14 编队
	CUnitSet			ukn_set[0xA];			// 0xDC
	unsigned char		ukn_0x1A4[0x3C];
	CUnit*				active_unit;			// 0x1E0
	CItem*				select_item;			// 0x1E4
	CUnitSet			select_group;			// 0x1E8
	unsigned char		ukn_0x1FC[0x10];
	CUnitSet*			active_group;			// 0x20C
};




struct CTriggerWar3 {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	unsigned char		ukn_0x8[0x4];
	HashGroup			agent_hash;				// 0xC
	unsigned char		ukn_0x14[0x34];
	Integer				ukn_integer_0x48;		// 0x48
	unsigned char		ukn_0x58[0x4];	
	uint32_t			execute_count;			// 0x5C
	unsigned char		ukn_0x60[0x4];
	Integer				enable;					// 0x64
	unsigned char		ukn_0x74[0x8];
}; // sizeof = 0x7C

struct CItem {
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x8];
	uint32_t			hash_0xC;				// 0xC
	uint32_t			hash_0x10;				// 0x10
	unsigned char		ukn_0x14[0xC];
	uint32_t			flag_0x20;				// 0x20
	unsigned char		ukn_0x24[0x4];
	CSpriteMini*		sprite;					// 0x28
	unsigned char		ukn_0x2C[0x4];
	uint32_t			type_id;				// 0x30
	unsigned char		ukn_0x34[0x24];
	float				life;					// 0x58 当前生命值
	uint32_t			flag_0x5C;				// 0x5C
	unsigned char		ukn_0x60[0x28];
	uint32_t			first_ability_id;		// 0x88
	HashCargo			owner_hash;				// 0x8C
	uint32_t			ability_count;			// 0x98
	HashCargo			ability_hash[4];		// 0x9C
	uint32_t			drop_id;				// 0xCC 重生神符产生的单位类型
};

struct ItemDefData {
	unsigned char		ukn_0x0[0x14];
	uint32_t			base_id;				// 0x14
	unsigned char		ukn_0x18[0x8];
	uint32_t			gold_cost;				// 0x20
	uint32_t			lumber_cost;			// 0x24
	uint32_t			stock_max;				// 0x28
	unsigned char		ukn_0x2C[0xC];
	uint32_t			level;					// 0x38
	uint32_t			item_class;				// 0x3C
	unsigned char		ukn_0x40[0x4];
	uint32_t			morph;					// 0x44 有效的物品转换目标
	uint32_t			pick_random;			// 0x48 可作为随机物品
	uint32_t			use_on_pickup;			// 0x4C 捡取时自动使用
	uint32_t			sellable;				// 0x50 可被市场出售
	uint32_t			pawnable;				// 0x54 可以被抵押
	uint32_t			usable;					// 0x58 主动使用
	uint32_t			perishable;				// 0x5C 使用完消失
	uint32_t			droppable;				// 0x60 可以丢弃
	uint32_t			drop_on_death;			// 0x64 死亡掉落
	unsigned char		ukn_0x68[0x4];
	uint32_t			uses;					// 0x6C 使用次数
	uint32_t			cooldown_id;			// 0x70 CD间隔组
	uint32_t			ignore_cd;				// 0x74 无视CD间隔
	unsigned char		ukn_0x78[0x20];
	RCString			abil_list;				// 0x98 技能列表
};

struct ItemUIDefData {
	unsigned char		ukn_0x0[0x2C];
	char**				name;					// 0x2C
	unsigned char		ukn_0x30[0x21C];
	char**				art;					// 0x24C
	unsigned char		ukn_0x250[0x10];
	char**				tip;					// 0x260
	unsigned char		ukn_0x264[0x8];
	char**				ubertip;				// 0x26C
	unsigned char		ukn_0x270[0x8];
	uint32_t			hotkey;					// 0x278
};
















// 这段布局已与 0x6F6BB2C0 / 0x6F6BB3A0 对齐，可视为 CAttachedEffect 基类。
struct CEffect {
	void**				vf_table;				// 0x0
	void*				ref_count;				// 0x4
	unsigned char		ukn_0x8[0x4];
	int32_t				hash_0xC;				// 0xC
	int32_t				hash_0x10;				// 0x10
	unsigned char		ukn_0x14[0xC];
	uint32_t			attach_flags;			// 0x20
	unsigned char		ukn_0x24[0x4];
	CModelInstance*		model_instance;			// 0x28 特效自己的模型实例
	unsigned char		ukn_0x2C[0x20];		// 0x2C 生命周期定时器对象
	uint32_t			attach_point_count;		// 0x4C 最多 10 个挂点ID
	uint32_t			attach_point_ids[0xA];	// 0x50
	HashGroup			bound_agent_hash;		// 0x78 从 +agl 对象复制来的目标哈希
};

struct CEffectFloating : public CEffect {
	unsigned char		ukn_0x80[0x4];
	int16_t				floating_anim_state;	// 0x84 浮动特效初始化时会清零
};

using CAttachedEffect = CEffect;
using CAttachedEffectStatic = CEffect;
using CAttachedEffectFloating = CEffectFloating;









struct DamageEventData {
	CUnit*				source;					// 0x0
	uint32_t			weapon_type;			// 0x4
	const char*			missile_path;			// 0x8
	uint32_t			flag;					// 0xC
	float				damage;					// 0x10
	uint32_t			damage_type;			// 0x14
	uint32_t			ukn_0x18;
	uint32_t			ukn_0x1C;
	uint32_t			attack_type;			// 0x20

	// 构造
	DamageEventData(CUnit* source_, uint32_t weapon_type_, uint32_t flag_, float damage_, uint32_t attack_type_, uint32_t damage_type_)
		: source(source_)
		, weapon_type(weapon_type_)
		, missile_path(nullptr)
		, flag(flag_)
		, damage(damage_)
		, damage_type(damage_type_)
		, ukn_0x18(0)
		, ukn_0x1C(0)
		, attack_type(attack_type_)
	{}
};


struct CMouseEvent {
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x4];
	uint32_t			event_id;				// 0x8
	void*				event_obj;				// 0xC
	unsigned char		ukn_0x10[0x4];
	uint32_t			cur_key;				// 0x14
	uint32_t			remain_key;				// 0x18
	unsigned char		ukn_0x1C[0x8];
	float				x;						// 0x24
	float				y;						// 0x28
	int32_t				scroll_value;			// 0x2C
};


struct CSpriteClickEvent {
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x4];
	uint32_t			event_id;				// 0x8
	unsigned char		ukn_0xC[0x10];
	float				x;						// 0x1C
	float				y;						// 0x20
	unsigned char		ukn_0x24[0x4];
	uint32_t			key;					// 0x28
	//uintptr_t			target_sprite;			// 0x2C
	//uintptr_t			target;					// 0x30
};

struct CSpriteTrackEvent {
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x4];
	uint32_t			event_id;				// 0x8
	CWorldFrameWar3*	world_frame_war3;		// 0xC
	unsigned char		ukn_0x10[0xC];
	float				x;						// 0x1C
	float				y;						// 0x20
	unsigned char		ukn_0x24[0x4];
	CGxDevice*			device;					// 0x28
	CSprite*			sprite;					// 0x2C
	void*				target;					// 0x30
	BOOL				is_enter;				// 0x34 1 - 移入; 0 - 移出
};

struct CTerrainClickEvent {
	void** vf_table;				// 0x0
	unsigned char		ukn_0x4[0x4];
	uint32_t			event_id;				// 0x8
	unsigned char		ukn_0xC[0x8];
	float				screen_x;				// 0x14
	float				screen_y;				// 0x18	
	float				x;						// 0x1C
	float				y;						// 0x20
	unsigned char		ukn_0x24[0x4];
	uint32_t			key;					// 0x28
	//unsigned char		ukn_0x2C[0x20];
	//uint32_t			order_id;				// 0x4C
	//unsigned char		ukn_0x50[0x8];
	//CTargetMode*		target_mode;			// 0x58
};

struct CMiniMapClickEvent {
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x4];
	uint32_t			event_id;				// 0x8
	CMiniMap*			mini_map;				// 0xC
	uint32_t			key;					// 0x10
	float				x;						// 0x14
	float				y;						// 0x18
};

struct CNetEventPlayerLeave {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	uint32_t			event_id;				// 0x8 0x4009007A
	unsigned char		ukn_0xC[0x8];
	uint32_t			flag_0x14;				// 0x14
	uint32_t			other_id;				// 0x18 通过0x30E020得到离开的玩家/未离开的玩家
	uint32_t			flag_0x1C;				// 0x1C 掉线1 退房7
};





struct COrder {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	unsigned char		ukn_0x8[0x4];
	uint32_t			hash_0xC;				// 0xC
	uint32_t			hash_0x10;				// 0x10
	unsigned char		ukn_0x14[0x10];
	uint32_t			order_id;				// 0x24
	uint32_t			player_id;				// 0x28
};



struct CFramePointAbsolute {
	void**						vf_table;				// 0x0
	float						x;						// 0x4
	float						y;						// 0x8
};// sizeof >= 0x10

struct CFramePointRelative {
	void**						vf_table;				// 0x0
	void*						frame;					// 0x4
	uint32_t					point;					// 0x8 锚点
	float						x;						// 0xC
	float						y;						// 0x10
};// sizeof = 0x504

struct CLayer {
	CObserver					observer;				// 0x0
	uint32_t					layer_style;			// 0xC layer标志
	unsigned char				ukn_0x10[0x44];
	uint32_t					visible;				// 0x54
	unsigned char				ukn_0x58[0x50];
	uint32_t					priority;				// 0xA8
	unsigned char				ukn_0xAC[0x4];
	uint32_t					flag_0xB0;				// 0xB0
};// sizeof = 0xB4

struct CFrame {
	CLayer						layer_0x0;				// 0x0
	CLayer						layer_0xB4;				// 0xB4
};// sizeof = 0x168

struct CLayoutFrame {
	void**						vf_table;				// 0x0
	uint32_t					point_count;			// 0x4	= 9
	void*						point[9];				// 0x8 对应9个锚点
	unsigned char				ukn_0x2C[0x10];
	CLayoutFrame*				self_point;				// 0x3C	=this_3C
	unsigned char				ukn_0x40[0x4];
	float						border_bottom;			// 0x44
	float						border_left;			// 0x48
	float						border_top;				// 0x4C
	float						border_right;			// 0x50
	uint32_t					visible;				// 0x54 隐藏
	float						width;					// 0x58
	float						height;					// 0x5C
	float						scale;					// 0x60
	unsigned char				ukn_0x64[0x4];
};

struct SimpleTopButtonList {
	void* button;					// 0x0
	uint32_t					button_type;			// 0x4
};

struct SimpleTopButtonListArray {
	SimpleTopButtonList* button_list[0x300];		// 0x0
};

struct SimpleFrameNode {
	void*		frame;	// 0x0
	uint32_t	type;	// 0x4
};

//一个全局对象指针GLOBAL_SIMPLETOP
struct CSimpleTop {
	void**						vf_table;				// 0x0
	unsigned char				ukn_0x4[0x168];
	void*						last_cursor_frame;		// 0x16C
	void*						last_down_frame;		// 0x170
	unsigned char				ukn_0x174[0x774];
	float						cursor_x;				// 0x8E8
	float						cursor_y;				// 0x8EC
	unsigned char				ukn_0x8F0[0xC];
};

struct CSimpleFrame {
	CLayoutFrame				layout_frame;			// 0x0
	CSimpleTop*					simple_top;				// 0x68	= *6FACE758			SimpleFrame
	CLayoutFrame*				parent_frame;			// 0x6C
	unsigned char				ukn_0x70[0x24];
	BOOL						show;					// 0x94
	unsigned char				ukn_0x98[0x10];
	uint32_t					priority;				// 0xA8
	unsigned char				ukn_0xAC[0x20];
	SimpleFrameTextureSettings* texture_settings;		// 0xCC	= 0					SimpleFrame, CToolTipWar3
	unsigned char				ukn_0xD0[0x54];
};

struct CSimpleRegion {
	CLayoutFrame				layout_frame;			// 0x0		
	uint32_t					color;					// 0x68
	unsigned char				ukn_0x6C[0x8];
	CLayoutFrame*				parent_frame;			// 0x74
	unsigned char				ukn_0x78[0xC];
}; // sizeof = 0x84

struct CSimpleTexture {
	CSimpleRegion				simple_region;			// 0x0
	unsigned char				ukn_0x84[0xC];
	uint32_t					blend_mode;				// 0x90
	unsigned char				ukn_0x94[0x54];
}; // sizeof = 0xE8

struct SimpleFrameTextureSettings {
	CSimpleTexture*				background_texture;		// 0x0
	CSimpleTexture*				left_texture;			// 0x4
	CSimpleTexture*				right_texture;			// 0x8
	CSimpleTexture*				top_texture;			// 0xC
	CSimpleTexture*				bottom_texture;			// 0x10
	RCString					background_path;		// 0x14	其实是RCStaticString，只是vtable不同	背景资源路径
	RCString					border_path;			// 0x20	其实是RCStaticString，只是vtable不同	边框资源路径
	uint32_t					flag_0x2C;				// 0x2C	border flag
	BOOL						is_tile;				// 0x30
	byte_t						ukn_0x34[0x4];
	float						border_size;			// 0x38	0.025	UnitTip=0.01
	byte_t						ukn_0x3C[0x4];
	float						top_padding;			// 0x40	0.0	填充	UnitTip = 0.0019
	float						bottom_padding;			// 0x44	0.0	填充 ^
	float						left_padding;			// 0x48	0.0	填充 ^
	float						right_padding;			// 0x4C	0.0	填充 ^
};

struct CModelFrame {
	CFrame						frame;					// 0x0
	unsigned char				ukn_0x16C[0xC];
};//sizeof = 0x178

struct CBackDropFrame {
	CModelFrame					model_frame;			// 0x0
	unsigned char				ukn_0x178[0x64];
};//sizeof = 0x1DC

struct CEditBox {
	CFrame						frame;					// 0x0

	// ...待定
};

struct TEXTBLOCK {
	void** vf_table;				// 0x0

	// ...待定
};

struct CSimpleFontString {
	CSimpleRegion				simple_region;			// 0x0
	byte_t						ukn_0x84[0xC];
	float						height;					// 0x90		?? = fontHeight * scale
	byte_t						ukn_0x94[0x8];
	const char*					str;					// 0x9C
	TEXTBLOCK*					text_block;				// 0xA0
	byte_t						ukn_0xA4[0x8];
	uint32_t					shadow_color;			// 0xAC
	float						shadow_x;				// 0xB0
	float						shadow_y;				// 0xB4
	byte_t						ukn_0xB8[0x8];
	uint32_t					flag_0xC0;				// 0xC0	= 0x212				
};//sizeof = 0xC4

struct CSimpleButton {
	CSimpleFrame				simple_frame;			// 0x0
	void*						click_event_observer;	// 0x124	CGameUI*?
	uint32_t					click_event_id;			// 0x128
	void*						mouse_event_observer;	// 0x12C	6F603060 CGameUI*?
	uint32_t					mouse_overevent_Id;		// 0x130
	uint32_t					mouse_outevent_Id;		// 0x134
	uint32_t					state;					// 0x138	1 = 亮图标; 0 = 暗图标
	uint32_t					avaliability;			// 0x13C
	uint32_t					mouse_button_flag;		// 0x140	= 0x10	指定什么鼠标键可以触发按钮
	unsigned char				ukn_0x144[0x14];
	CSimpleTexture*				texture_disable;		// 0x158
	CSimpleTexture*				texture_enable;			// 0x15C
	CSimpleTexture*				texture_push;			// 0x160
	CSimpleTexture*				cur_texture;			// 0x164
}; //sizeof = 0x168

struct CSimpleConsole {
	CSimpleFrame				simple_frame;			// 0x0

	// ...待定
};

struct CSimpleStatusBar {
	CSimpleFrame				simple_frame;			// 0x0
	uint32_t					changed_value;			// 0x124 ?
	float						min_value;				// 0x128
	float						max_value;				// 0x12C
	float						cur_value;				// 0x130
	CSimpleTexture* texture;				// 0x134
};//sizeof = 0x138

struct CSlider {
	void** vf_table;				// 0x0
	unsigned char				ukn_0x4[0x1E8];
	float						min_value;				// 0x1EC
	float						max_value;				// 0x1F0
	float						cur_value;				// 0x1F4
};	// ...待定

struct CSpriteFrame {
	CFrame						frame;					// 0x0
	unsigned char				ukn_0x168[0x10];
	CSpriteUber**				sprite;					// 0x178
	unsigned char				ukn_0x17C[0x34];
};//sizeof = 0x1B0

struct CStatBar {
	CSimpleStatusBar			simple_status_bar;		// 0x0
	uint32_t					type;					// 0x138 type
	unsigned char				ukn_0x13C[0x4];
	CSimpleFontString* simple_font_string;		// 0x140 XP bar text
	CSimpleTexture* simple_texture;			// 0x144
	unsigned char				ukn_0x148[0x8];
	float						offset;					// 0x150 疑似血条偏移程度?
	CUnit* owner;					// 0x154
};//sizeof = 0x158

struct CTextArea {
	CFrame						frame;

	// ...待定
};

struct CHighlightFrame {
	CFrame						frame;					// 0x0
	unsigned char				ukn_0x168[0x20];
	char						texture[0x104];			// 0x188 
	unsigned char				ukn_0x28C[0x18];
}; //sizeof = 0x2A4

struct CControl {
	CSpriteFrame				sprite_frame;			// 0x0
	unsigned char				ukn_0x1B0[0x4];
	CHighlightFrame*			highlight_frame;		// 0x1B4
	unsigned char				ukn_0x1B8[0x4];
	CBackDropFrame*				backdrop_frame_0x1BC;	// 0x1BC
	CBackDropFrame*				backdrop_frame_0x1C0;	// 0x1C0
	CBackDropFrame*				backdrop_frame_0x1C4;	// 0x1C4
	CBackDropFrame*				backdrop_frame_0x1C8;	// 0x1C8
	unsigned char				ukn_0x1CC[0x18];
}; //sizeof = 0x1E4

struct CTextStyle {
	uint32_t					flag;					// 0x0
	uint32_t					ukn_0x4;
};

struct CTextFrame {
	CControl					control;				// 0x0
	uint32_t					str_length;				// 0x1E4
	const char*					str;					// 0x1E8
	byte_t						ukn_0x1EC[0x4];
	CTextStyle*					style;					// 0x1F0
	uint32_t					color;					// 0x1F4
	uint32_t					highlight_color; 		// 0x1F8
	uint32_t					disabled_color; 		// 0x1FC
	uint32_t					shadow_color; 			// 0x200
	TEXTBLOCK*					text_block;				// 0x204
	byte_t						ukn_0x208[0x8];
	float						shadow_x;				// 0x210
	float						shadow_y;				// 0x214
	float						font_char_spacing;		// 0x218
	byte_t						ukn_0x21C[0x8];
	BOOL						need_update_align;		// 0x224
	uint32_t					flag_0x228;				// 0x228
	byte_t						ukn_0x22C[0x10];		
	uint32_t					need_update;			// 0x23C
}; //sizeof = 0x240





struct CPortraitButton {
	void**						vf_table;				// 0x0
	uint32_t					ref_count;				// 0x4
	unsigned char				ukn_0x8[0x4];
	uint32_t					flag_0xC;				// 0xC
	unsigned char				ukn_0x10[0x130];
	CCamera*					camera;					// 0x140
	unsigned char				ukn_0x144[0xF4];
	CUnit*						portrait_unit;			// 0x238
	uint32_t					portrait_unit_id;		// 0x23C
	CSimpleFontString*			hp_text;				// 0x240
	CSimpleFontString*			mp_text;				// 0x244
	float						cur_hp;					// 0x248
	float						max_hp;					// 0x24C
	float						cur_mp;					// 0x250
	float						max_mp;					// 0x254
}; // sizeof = 0x264



struct CCamera {
	void**						vf_table;				// 0x0
	uint32_t					ref_count;				// 0x4
	unsigned char				ukn_0x8[0x34];
	float						camera_x;				// 0x3C
	float						camera_y;				// 0x40
	float						camera_z;				// 0x44
	unsigned char				ukn_0x48[0x1C];
	float						focus_x;				// 0x64
	float						focus_y;				// 0x68
	float						focus_z;				// 0x6C
};



struct CRenderListNode {
	CRenderListNode*				last_node;				// 0x00
	CRenderListNode*				next_node;				// 0x04
	void*							data;					// 0x08
	uint32_t						category_flag;			// 0x0C
}; // sizeof = 0x10

struct ListHeader {
	uint32_t					reserved0[0x3];			// 0x00 ~ 0x08
	void*						data;					// 0x0C	条目数组指针
	uint32_t					reserved10;				// 0x10
	uint32_t					count;					// 0x14	元素数量
}; // sizeof = 0x18

struct WorldObjectListEntry {
	void*						world_object_entry;		// 0x00
	uint32_t					ukn_0x04;				// 0x04
	uint32_t					ukn_0x08;				// 0x08
	uint32_t					ukn_0x0C;				// 0x0C
	uint32_t					ukn_0x10;				// 0x10
	uint32_t					handle_or_unit;			// 0x14	常见为 handleId / unit 指针通道
}; // sizeof = 0x18

struct WorldObjectEntry {
	void**						vf_table;				// 0x00
	unsigned char				ukn_0x04[0x1C];
	void*						scene_node;				// 0x20
}; // sizeof = 0x24

struct SceneNodeTintRecord {
	uint32_t					rgba_base;				// 0x00
	uint8_t						mod_r;					// 0x04
	uint8_t						mod_g;					// 0x05
	uint8_t						mod_b;					// 0x06
	uint8_t						ukn_0x07;				// 0x07
	unsigned char				ukn_0x08[0x8];
}; // sizeof = 0x10

struct GxStagePresetRecord {
	// 当前已确认这 48 字节会参与 3x4 变换矩阵乘法、按轴缩放和点变换。
	unsigned char				raw[0x30];
}; // sizeof = 0x30

struct RenderOverridePresetChannelDataRecord {
	float						channel0_float3_data[3];// 0x00
	float						channel1_vec4_data[4];	// 0x0C
	float						channel2_float3_data[3];// 0x1C
}; // sizeof = 0x28

struct RenderOverridePresetChannelResolverRecord {
	unsigned char				channel0_resolver[0x1C];// 0x00
	unsigned char				channel1_resolver[0x1C];// 0x1C
	unsigned char				channel2_resolver[0x1C];// 0x38
}; // sizeof = 0x54

struct RenderOverrideTransformAuxRecord {
	float						resolved_vec3_0[3];		// 0x00
	float						resolved_vec4_1[4];		// 0x0C
	float						resolved_vec3_2[3];		// 0x1C
	float						backup_vec3_0[3];		// 0x28
	float						backup_vec4_1[4];		// 0x34
	float						backup_vec3_2[3];		// 0x44
}; // sizeof = 0x50

struct RenderOverrideDependencyGateOutputRecord {
	uint8_t						color_rgb[3];			// 0x00
	uint8_t						enable_or_alpha;		// 0x03
	uint32_t					ukn_0x04;				// 0x04
	float						resolved_weight;		// 0x08
	float						alpha_scale;			// 0x0C
}; // sizeof = 0x10

struct RenderOverrideCompactScalarOutputEntry {
	int32_t						value;					// 0x00 默认常见为 -1
}; // sizeof = 0x04

struct RenderOverrideGraphOutputBundle {
	unsigned char				ukn_0x00[0x08];
	GxStagePresetRecord*		primary_preset_outputs;	// 0x08 主 48B preset 输出表
	unsigned char				ukn_0x0C[0x08];
	void*						gxu_light_array_handle;	// 0x14 +0x08 -> CGxuLight*[]
	void*						particle_emitter_runtime_array_handle; // 0x18 +0x08 -> CParticleEmitterRuntime[]
	unsigned char				ukn_0x1C[0x08];
	GxStagePresetRecord*		shared_preset_outputs;	// 0x24 node_type=2 写入的共享 preset 输出
	unsigned char				ukn_0x28[0x04];
	RenderOverrideDependencyGateOutputRecord* dependency_gate_outputs; // 0x2C 16B gate 输出表
	unsigned char				ukn_0x30[0x08];
	void*						visibility_byte_outputs_handle; // 0x38 +0x08 -> byte/flag 输出缓冲
	RenderOverrideCompactScalarOutputEntry* compact_scalar_outputs; // 0x3C 4B stride 紧凑标量输出
}; // sizeof >= 0x40

struct RenderStagePresetOverrideNode {
	unsigned char				ukn_0x00[0xA8];
	uint32_t					source_record_index;	// 0xA8 source point/vector/light 记录索引，node_type 1/2/7 都会消费它
	uint32_t					output_slot_index;		// 0xAC 目标输出槽/部件索引
	uint8_t						node_type;				// 0xB0 节点类型码
	uint8_t						mode_bits;				// 0xB1 模式位（0x38/0x40 等）
	unsigned char				ukn_0xB2[0x06];
	uint32_t					child_count;			// 0xB8
	void*						child_array;			// 0xBC 子节点数组
	unsigned char				ukn_0xC0[0x04];
	uint8_t						visibility_mask_index;	// 0xC4 0xFF=直接可见，否则查状态掩码表
}; // sizeof >= 0xC5

struct ParticleEmitterOverrideSlot {
	unsigned char				ukn_0x00[0x70];
	unsigned char				gate_resolve_state[0x0C];// 0x70 sub_6F77A940 读取的门控/可见性状态块
	float						emission_scale;			// 0x7C 传给 CParticleEmitterRuntime_UpdateAndRender
}; // sizeof = 0x80

struct PlaneParticleEmitterOverrideSlot {
	unsigned char				ukn_0x00[0x58];
	unsigned char				gate_resolve_state[0x30];// 0x58 sub_6F77D350 读取的门控/权重状态块
	void*						plane_particle_emitter; // 0x88 CPlaneParticleEmitter*
}; // sizeof = 0x8C

struct CAnimRibbonObjStatus;
using AnimRibbonOverrideSlot = CAnimRibbonObjStatus;

struct CModelPartSequenceDefHeader {
	unsigned char				ukn_0x00[0x18];
	void*						sequence_frame_records;	// 0x18 stride 0x8C
	uint32_t*					wrapped_frame_periods;	// 0x20 per-channel period/modulus 表
}; // sizeof >= 0x24

struct CModelPartFrameWindowRecord {
	int32_t						current_frame_index;	// 0x00
	int32_t						frame_state_with_loop_bit;// 0x04 bit31=loop/wrap 命中
	void*						on_part_loop_or_complete;// 0x08
	void*						on_part_loop_or_complete_ctx;// 0x0C
}; // sizeof = 0x10

struct CModelPartSequenceFrameRecord {
	int32_t						frame_start;			// 0x00
	int32_t						frame_end;				// 0x04
	unsigned char				ukn_0x08[0x04];
	uint32_t					flags;					// 0x0C bit0=loop时 clamp；否则 wrap
	unsigned char				ukn_0x10[0x7C];
}; // sizeof = 0x8C

struct CModelPartStateDefRecord {
	unsigned char				ukn_0x00[0xE0];
	uint8_t						visibility_dependency_index; // 0xE0
	unsigned char				ukn_0xE1[0x03];
}; // sizeof = 0xE4

struct CModelPartWeightVisibilityRecord {
	unsigned char				ukn_0x00[0x34];
	float						resolved_weight;		// 0x34 >0 才允许参与递归/构建/提交
}; // sizeof = 0x38

struct CModelVisibilityDependencyRecord {
	unsigned char				ukn_0x00[0x18];
	uint32_t					flags;					// 0x18 bit0=dependency ready/visible
}; // sizeof = 0x1C

struct CModelPartStateController {
	unsigned char				ukn_0x00[0x08];
	CModelPartFrameWindowRecord* frame_window_array;	// 0x08 stride 0x10
	unsigned char				ukn_0x0C[0x30];
	void*						global_loop_callback;	// 0x3C
	void*						global_loop_callback_ctx;// 0x40
	CModelPartSequenceDefHeader* sequence_def_header;	// 0x44
	float						frame_time_scale;		// 0x48
	uint32_t					last_tick_ms;			// 0x4C
	int32_t						frame_delta;			// 0x50
	uint32_t					flags;					// 0x54
	uint8_t						current_part_index;		// 0x58
	unsigned char				ukn_0x59[0x03];
	uint32_t*					wrapped_frame_offsets;	// 0x5C
	uint32_t					wrapped_frame_offset_count;// 0x60
	unsigned char				ukn_0x64[0x30];
	ParticleEmitterOverrideSlot* particle_emitter_slots;// 0x94
	uint32_t					particle_emitter_slot_count;// 0x98
	PlaneParticleEmitterOverrideSlot* plane_particle_slots;// 0x9C
	uint32_t					plane_particle_slot_count;// 0xA0
	CAnimRibbonObjStatus*		anim_ribbon_statuses;	// 0xA4
	uint32_t					anim_ribbon_status_count;// 0xA8
}; // sizeof >= 0xAC

struct CGxuLight {
	uint32_t					state_flag;				// 0x00 sub_6F0CC5F0 直接返回
	uint32_t					use_input_point_transform;// 0x04
	float						position_or_direction[3];// 0x08
	uint32_t					ambient_color_packed;	// 0x14
	uint32_t					directional_color_packed;// 0x18
	float						ambient_intensity;		// 0x1C
	float						directional_intensity;	// 0x20
	uint32_t					ref_count;				// 0x24
	float						max_distance_or_range;	// 0x28 ctor 默认 +INF
}; // sizeof = 0x2C

struct RenderOverrideLocalPointOutputRecord {
	unsigned char				ukn_0x00[0x34];
	float						resolved_local_point[3];// 0x34
}; // sizeof = 0x40

struct CAnimRibbonObjStatus {
	unsigned char				ukn_0x00[0x28];
	unsigned char				gate_resolve_state[0x28];// 0x28 sub_6F77A940 读取的门控/可见性状态块
	unsigned char				ukn_0x50[0x20];
	void*						anim_ribbon_obj;		// 0x70 CAnimRibbonObj*
}; // sizeof = 0x74

struct CParticleEmitterRuntimeParticle {
	float						remaining_life;			// 0x00
	float						spawn_phase_offset;		// 0x04
	float						position[3];			// 0x08
	float						velocity[3];			// 0x14
	uint32_t					render_scale_or_size;	// 0x20
	void*						model_instance_or_sprite;// 0x24
}; // sizeof = 0x28

struct CParticleEmitterRuntime {
	unsigned char				ukn_0x00[0x04];
	float						spawn_accumulator;		// 0x04
	void*						spawn_ctx_a;			// 0x08
	void*						spawn_ctx_b;			// 0x0C
	float						emission_rate_scale;	// 0x10
	unsigned char				ukn_0x14[0x24];
	CParticleEmitterRuntimeParticle* particle_array;	// 0x38 stride 0x28
	unsigned char				ukn_0x3C[0x0C];
	uint32_t*					active_indices;			// 0x48
	unsigned char				ukn_0x4C[0x04];
	uint32_t					active_count;			// 0x50
	unsigned char				ukn_0x54[0x08];
	uint32_t*					free_indices;			// 0x5C
	unsigned char				ukn_0x60[0x04];
	uint32_t					free_count;				// 0x64
}; // sizeof = 0x68

struct CPlaneParticleEmitter {
	unsigned char				ukn_0x00[0x194];
	uint32_t					flags;					// 0x194 bit0/bit6/bit7/bit11 参与启停与矩阵更新
	float						world_matrix_3x4[12];	// 0x198
}; // sizeof >= 0x1C8

struct CAnimRibbonObj {
	unsigned char				ukn_0x00[0x18];
	uint32_t					last_tick_ms;			// 0x18
	float						phase_accumulator;		// 0x1C
	uint32_t					has_cached_transform;	// 0x20
	unsigned char				ukn_0x24[0x12C];
	uint32_t					flags;					// 0x150 bit0=enabled bit1=ready
	unsigned char				ukn_0x154[0x10];
	int32_t						ukn_0x164;				// 0x164 ctor 默认 -1
	int32_t						ukn_0x168;				// 0x168 ctor 默认 1
}; // sizeof = 0x16C

struct GeosetPrimitiveRecord {
	uint32_t					primitive_type_or_material_slot; // 0x00 126250 默认写 3
	uint32_t					index_count;          // 0x04
}; // sizeof = 0x08

struct GeosetUvLayerVec2ArrayRecord {
	uint32_t					capacity;             // 0x00
	uint32_t					count;                // 0x04
	float*						data;                 // 0x08 Vec2*；<=4 时回落到 inline_storage
	float						inline_storage[8];    // 0x0C 4 * vec2
}; // sizeof = 0x2C

struct CModelGeosetBindingRecord {
	int32_t						binding_value_0;      // 0x00 sub_6F12A6A0 写入 a12
	int32_t						binding_value_1;      // 0x04 默认 -1
	float						scale_or_weight_0;    // 0x08 默认 1.0
	float						scale_or_weight_1;    // 0x0C 默认 1.0
}; // sizeof = 0x10

struct CGeosetData {
	void**						vf_table;             // 0x00
	uint32_t					ref_count;            // 0x04
	uint32_t					vertex_capacity;      // 0x08
	uint32_t					vertex_count;         // 0x0C
	float*						vertex_positions;     // 0x10 Vec3*，stride 0x0C
	unsigned char				ukn_0x14[0x30];
	uint32_t					vertex_group_capacity; // 0x44
	uint32_t					vertex_group_count;   // 0x48
	uint8_t*					vertex_group_indices; // 0x4C 每顶点 1B，指向 matrix-group slot
	uint32_t					normal_capacity;      // 0x50
	uint32_t					normal_count;         // 0x54
	float*						normal_vectors;       // 0x58 Vec3*，stride 0x0C
	unsigned char				ukn_0x5C[0x30];
	uint32_t					uv_layer_capacity;    // 0x8C
	uint32_t					uv_layer_count;       // 0x90
	GeosetUvLayerVec2ArrayRecord* uv_layers;         // 0x94 stride 0x2C
	unsigned char				ukn_0x98[0x2C];
	uint32_t					primitive_record_capacity; // 0xC4
	uint32_t					primitive_record_count;    // 0xC8
	GeosetPrimitiveRecord*		primitive_records;    // 0xCC stride 0x08
	uint32_t					index_capacity;       // 0xD8
	uint32_t					index_count;          // 0xDC
	uint16_t*					index_buffer_u16;     // 0xE0
	unsigned char				ukn_0xE4[0x08];
	uint32_t					matrix_group_size_capacity; // 0xEC
	uint32_t					matrix_group_count;   // 0xF0
	uint32_t*					matrix_group_sizes;   // 0xF4 每组包含多少 bone/matrix index
	uint32_t					matrix_index_capacity; // 0xF8
	uint32_t					matrix_index_count;   // 0xFC
	uint32_t*					matrix_indices;       // 0x100 扁平 bone/matrix index 表
	uint32_t					layout_or_material_meta_0; // 0x104
	uint32_t					layout_or_material_slot;   // 0x108 126250 末参写入
	unsigned char				ukn_0x10C[0x10];
	uint32_t					merged_geoset_slot_or_binding_index; // 0x11C 131320 写入
}; // sizeof >= 0x120

struct CGeoset {
	void**						vf_table;             // 0x00
	uint32_t					ref_count;            // 0x04
	int32_t						handle_id;            // 0x08 默认 -1
	CGeosetData*				geoset_data;          // 0x0C
	uint32_t					material_or_layout_slot; // 0x10 1325E0 从 source+0x10 复制
}; // sizeof >= 0x14

struct CModel {
	void**						vf_table;             // 0x00
	uint32_t					ref_count;            // 0x04
	unsigned char				ukn_0x08[0x54];
	uint32_t					final_pose_matrix_count; // 0x5C sub_6F12FDC0 直接消费
	GxStagePresetRecord*		final_pose_matrix_array; // 0x60 stride 0x30 的最终 3x4 palette
	float						world_matrix_3x4[12]; // 0x64
	uint32_t					flags;                // 0x94 bit2=non-unit-scale bit4=complex tree
	void*						part_state_controller; // 0x98 CModelPartStateController*
	void*						owned_model_data_handle; // 0x9C retained HMODELDATA / shared resource
	unsigned char				ukn_0xA0[0x24];
	uint32_t					child_runtime_bucket_count; // 0xC4
	SceneNodeChildBucket*		child_runtime_buckets; // 0xC8
	unsigned char				ukn_0xCC[0x08];
	uint8_t*					child_part_eligibility_cache; // 0xD4
}; // sizeof >= 0xD8

struct CModelData {
	void**						vf_table;             // 0x00
	uint32_t					ref_count;            // 0x04
	uint32_t					geoset_capacity;      // 0x08
	uint32_t					geoset_count;         // 0x0C
	CGeoset**					geosets;              // 0x10
	uint32_t					geoset_growth;        // 0x14
	uint32_t					geoset_binding_capacity; // 0x18
	uint32_t					geoset_binding_count; // 0x1C
	CModelGeosetBindingRecord*	geoset_bindings;      // 0x20 stride 0x10
	uint32_t					geoset_binding_growth; // 0x24
	uint32_t					material_capacity;    // 0x28
	uint32_t					material_count;       // 0x2C
	void**						material_handles;     // 0x30 HMATERIAL*
	uint32_t					material_growth;      // 0x34
	unsigned char				ukn_0x38[0x10];
	uint32_t					material_slot_capacity; // 0x48
	uint32_t					material_slot_count;  // 0x4C
	uint8_t*					material_slot_bytes;  // 0x50
	uint32_t					material_slot_growth; // 0x54
	unsigned char				ukn_0x58[0x3C];
	uint32_t					flags;                // 0x94 bit4=complex model path
	void*						shared_resource_proto; // 0x98 130CD0 通过 785B30 retain
	void*						model_data_handle;    // 0x9C 130CD0 通过 04F200 retain
	unsigned char				ukn_0xA0[0x18];
	uint32_t					light_template_count; // 0xB8
	void**						light_templates;      // 0xBC
	unsigned char				ukn_0xC0[0x08];
	void*						child_runtime_group_records; // 0xC8 12B stride 组记录表
	unsigned char				ukn_0xCC[0x04];
	uint32_t					child_runtime_group_count; // 0xD0
	unsigned char				ukn_0xD4[0x10];
	uint32_t					plane_particle_emitter_count; // 0xE8
	void**						plane_particle_emitters; // 0xEC
	unsigned char				ukn_0xF0[0x0C];
	uint32_t					camera_count;         // 0x100
	void**						cameras;              // 0x104
}; // sizeof >= 0x108

struct SceneNodeChildLink {
	int32_t						ukn_0x00;				// 0x00
	SceneNodeChildLink*			next_link;				// 0x04
	void*						child_scene;			// 0x08 SceneNode*；动画/stage-preset 递归也会复用
	uint32_t					link_flags;				// 0x0C bit0=允许递归传播到 child_scene
}; // sizeof = 0x10

struct SceneNodeChildBucket {
	uint32_t					ukn_0x00;				// 0x00
	uint32_t					ukn_0x04;				// 0x04
	SceneNodeChildLink*			first_link;				// 0x08
}; // sizeof = 0x0C

struct SceneNode {
	void*						vf_table;				// 0x00
	unsigned char				ukn_0x04[0x8];
	uint32_t					renderable_count;		// 0x0C
	void**						renderable_list;		// 0x10
	unsigned char				ukn_0x14[0x0C];
	SceneNodeTintRecord*		cull_table;				// 0x20 16 字节颜色/剔除记录
	unsigned char				ukn_0x24[0x0C];
	void**						mesh_info_table;		// 0x30 MeshInfo*[]
	unsigned char				ukn_0x34[0x1C];
	uint8_t*					visibility_table;		// 0x50 layer 可见性/alpha 表
	unsigned char				ukn_0x54[0x10];
	float						world_matrix[12];		// 0x64 3x4
	uint32_t					flags;					// 0x94 bit4=存在透明附加链
	void*						child_visibility_ctx;	// 0x98
	void*						world;					// 0x9C
	uint32_t					stage_preset_base_index;// 0xA0 共享 stage preset 池起始 index
	unsigned char				ukn_0xA4[0x20];
	uint32_t					child_count;			// 0xC4
	SceneNodeChildBucket*		child_buckets;			// 0xC8 stride=0x0C
	uint8_t*					child_visibility_cache;	// 0xD4 动画/stage-preset 递归也会复用
}; // sizeof >= 0xD8

struct RenderBatchElement {
	void*						batch_entry;			// 0x00 RenderablePart*
	uint32_t					flags;					// 0x04 bit0=meshFlag bit1=后续仍有可见层
	uint32_t					layer_index;			// 0x08
	uint32_t					layer_counter;			// 0x0C
	void*						layer_state_ptr;		// 0x10 MeshLayerStateRecord*
}; // sizeof = 0x14

struct RenderablePart {
	unsigned char				ukn_0x00[0x08];
	uint32_t					stage_preset_span_base_index;	// 0x08 CModel_AssignVisiblePartStagePresetSpan 写入的共享 preset span 起始 index
	void*						mesh_data;				// 0x0C MeshData*
	uint32_t					skip_flag;				// 0x10 非 0 时跳过/禁用
	SceneNode*					scene_node;				// 0x14 由 RenderBatch_Submit 回写
}; // sizeof >= 0x18

struct MeshLayerStateRecord {
	uint32_t					primary_resource_binding;	// 0x00 传给主资源绑定 helper 的 layer 级主绑定对象
	uint32_t					state_words_0x04[5];	// 0x04
	uint32_t					blend_or_draw_mode;		// 0x18 ==4 时 special alpha 走白色路径
	uint32_t					aux_ref_enable_0;		// 0x1C
	uint32_t					aux_ref_enable_1;		// 0x20
}; // sizeof = 0x24

struct MeshLayerDispatchRecord {
	unsigned char				ukn_0x00[0x0C];
	int32_t						stage_preset_index_0;	// 0x0C stage slot0 选中的共享 preset
	int32_t						stage_preset_index_1;	// 0x10 stage slot1 选中的共享 preset
	int32_t						aux_ref_index_0;		// 0x14
	int32_t						aux_ref_index_1;		// 0x18
	uint32_t					visibility_offset;		// 0x1C
	uint32_t					alpha_flags;			// 0x20 bit0=双 alpha/color 提交
	uint32_t					stage_mode_0;			// 0x24 >=12 时 stage0 走共享 preset
	uint32_t					stage_mode_1;			// 0x28 >=12 时 stage1 走共享 preset
}; // sizeof = 0x2C

struct MeshAuxResourceEntry {
	unsigned char				ukn_0x00[0x08];
	uint32_t					resource_binding;		// 0x08
	unsigned char				ukn_0x0C[0x20];
}; // sizeof = 0x2C

struct LayerInfo {
	unsigned char				ukn_0x00[0x10];
	MeshLayerDispatchRecord*	layer_records;			// 0x10 stride=0x2C
}; // sizeof = 0x14

struct MeshInfo {
	unsigned char				ukn_0x00[0x0C];
	uint32_t					layer_count;			// 0x0C
	MeshLayerStateRecord*		layer_states;			// 0x10 stride=0x24
	unsigned char				ukn_0x14[0x24];
	LayerInfo*					layer_info;				// 0x38
}; // sizeof >= 0x3C

struct MeshData {
	void*						vf_table;				// 0x00
	unsigned char				ukn_0x04[0x08];
	uint32_t					primary_stream_arg0;	// 0x0C 传给多槽流绑定 helper 的主流参数0
	uint32_t					primary_stream_ptr;		// 0x10 主流顶点数据基址/绑定指针
	unsigned char				ukn_0x14[0x34];
	uint32_t					primary_stream_stride;	// 0x48 主流 stride
	uint32_t					stream1_ptr;			// 0x4C 第二个流/属性源指针
	unsigned char				ukn_0x50[0x08];
	uint32_t					stream1_stride;			// 0x58 第二个流/属性源 stride
	unsigned char				ukn_0x5C[0x38];
	MeshAuxResourceEntry*		aux_layer_resource_table;// 0x94
	unsigned char				ukn_0x98[0x30];
	uint32_t					sub_primitive_count;	// 0xC8
	void*						sub_primitive_pairs;	// 0xCC 每项 8 字节
	unsigned char				ukn_0xD0[0x10];
	uint32_t					primitive_base_index;	// 0xE0
	unsigned char				ukn_0xE4[0x0C];
	void*						transform_or_pose_ctx;	// 0xF0
	unsigned char				ukn_0xF4[0x10];
	uint32_t					mesh_flag;				// 0x104
	uint32_t					mesh_index;				// 0x108
	float						bounding_pos[3];		// 0x10C
	unsigned char				ukn_0x118[0x04];
	uint32_t					cull_index;				// 0x11C
	uint32_t					transparent_key;		// 0x120
	uint32_t					extra_mesh_flags;		// 0x124 bit2 影响 0x6F138510 过滤
}; // sizeof = 0x128

struct CWorldFrameWar3 {
	void**						vf_table;				// 0x0
	uint32_t					ref_count;				// 0x4
	unsigned char				ukn_0x8[0x144];
	CRenderListNode*				render_category_list;	// 0x14C	类别观察者链表头
	unsigned char				ukn_0x150[0x1C];
	ListHeader*					world_group_0;			// 0x16C
	ListHeader*					world_group_1;			// 0x170
	ListHeader*					world_group_2;			// 0x174
	unsigned char				ukn_0x178[0x24];
	CCameraWar3*				camera;					// 0x19C
	uint32_t					ukn_0x1A0;				// 0x1A0
	uint32_t					click_route_mask_a;		// 0x1A4
	uint32_t					click_route_mask_b;		// 0x1A8
	uint32_t					player_color_mode;		// 0x1AC
	uint32_t					player_color_stack_capacity;// 0x1B0
	uint32_t					player_color_stack_size;// 0x1B4
	uint32_t*					player_color_stack;		// 0x1B8
	uint32_t					player_color_stack_grow;// 0x1BC
	uint32_t					suppress_track_pop;		// 0x1C0
	uint32_t					world_frame_visible;	// 0x1C4
	RCString					player_color_label;		// 0x1C8
	float						min_y;					// 0x1D4
	float						min_x;					// 0x1D8
	float						max_y;					// 0x1DC
	float						max_x;					// 0x1E0
	unsigned char				ukn_0x1E4[0x5C];
	void*						tracked_sprite_handle;	// 0x240
	void*						tracked_sprite_ref;		// 0x244
	uint32_t					stage18_preview_enabled;// 0x248	stage18 前半段门控
	unsigned char				ukn_0x24C[0x4];
	void*						stage18_preview_ctx;	// 0x250
	unsigned char				render_scene_cleanup_context[0xA4]; // 0x254
	uint32_t					stage21_state_flags;	// 0x2F8
	uint32_t					stage21_indicator_ready;// 0x2FC
	int32_t						shadow_mode_or_stage21_listb_entry_index; // 0x300
	unsigned char				ukn_0x304[0xC];
	float						mouse_x;				// 0x310
	float						mouse_y;				// 0x314
	float						mouse_z;				// 0x318
	void*						active_render_queue;	// 0x31C
	uint32_t					input_suppressed;		// 0x320
	uint32_t					stage17_enabled;		// 0x324
	unsigned char				ukn_0x328[0xC];
	void*						terrain_fog_runtime;	// 0x334
	void*						day_night_terrain_runtime;// 0x338
	void*						day_night_unit_runtime;	// 0x33C
	void*						extra_environment_runtime;// 0x340
	unsigned char				ukn_0x344[0x4];
	RCString					ukn_rcstring_0x348;		// 0x348
	void*						stage0_context;			// 0x354
	uint32_t					stage0_gate_enabled;	// 0x358
	uint32_t					stage0_gate_ready;		// 0x35C
	unsigned char				ukn_0x360[0x28];
	uint32_t					scaled_anim_time_enabled;// 0x388
	uint32_t					ukn_0x38C;				// 0x38C
	float						scaled_anim_time_accum;	// 0x390
	float						day_hours_seed;			// 0x394
	uint32_t					misc_scaled_anim_time_key;// 0x398
	uint32_t					misc_day_hours_key;		// 0x39C
	uint32_t					color_friend_key;		// 0x3A0
	uint32_t					color_neutral_key;		// 0x3A4
	uint32_t					color_enemy_key;		// 0x3A8
	unsigned char				ukn_0x3AC[0x190];
	uint32_t					rally_indicator_dst_capacity;// 0x53C
	uint32_t					rally_indicator_dst_count;// 0x540
	void*						rally_indicator_dst_array;// 0x544
	unsigned char				ukn_0x548[0x40];
	void*						rally_indicator_src_object;// 0x588
	void*						target_point_confirm_object;// 0x58C
	uint32_t					waypoint_indicator_capacity;// 0x590
	uint32_t					waypoint_indicator_count;// 0x594
	void*						waypoint_indicator_array;// 0x598
	unsigned char				ukn_0x59C[0x8];
	uint32_t					deferred_callback_count_a;// 0x5A4
	void**						deferred_callback_array_a;// 0x5A8
	uint32_t					ukn_0x5AC;				// 0x5AC
	uint32_t					ukn_0x5B0;				// 0x5B0
	uint32_t					deferred_callback_count_b;// 0x5B4
	void**						deferred_callback_array_b;// 0x5B8
	uint32_t					ukn_0x5BC;				// 0x5BC
	uint32_t					ukn_0x5C0;				// 0x5C0
	uint32_t					deferred_callback_count_c;// 0x5C4
	void**						deferred_callback_array_c;// 0x5C8
	uint32_t					ukn_0x5CC;				// 0x5CC
	uint32_t					ukn_0x5D0;				// 0x5D0
	uint32_t					deferred_callback_count_d;// 0x5D4
	void**						deferred_callback_array_d;// 0x5D8
	uint32_t					ukn_0x5DC;				// 0x5DC
	uint32_t					ukn_0x5E0;				// 0x5E0
	uint32_t					deferred_callback_count_e;// 0x5E4
	void**						deferred_callback_array_e;// 0x5E8
	uint32_t					ukn_0x5EC;				// 0x5EC
	uint32_t					debug_overlay_toggle_primary;// 0x5F0
	uint32_t					deferred_callback_count_f;// 0x5F4
	void**						deferred_callback_array_f;// 0x5F8
	uint32_t					ukn_0x5FC;				// 0x5FC
	uint32_t					debug_overlay_toggle_secondary;// 0x600
	unsigned char				ukn_0x604[0x40];
	uint32_t					deferred_selection_count;// 0x644
	void**						deferred_selection_array;// 0x648
	unsigned char				ukn_0x64C[0x14];
	int32_t						current_category_mode;	// 0x660
	int32_t						current_render_category;// 0x664
};
static_assert(sizeof(CWidget) == 0x34, "CWidget size mismatch");
static_assert(sizeof(CSpriteAnimPayloadArray) == 0x10,
	"CSpriteAnimPayloadArray size mismatch");
static_assert(sizeof(CSpriteAnimRequest) == 0x1C,
	"CSpriteAnimRequest size mismatch");
static_assert(sizeof(CModelAnimController) == 0x9C,
	"CModelAnimController size mismatch");
static_assert(sizeof(CSprite) == 0x44, "CSprite size mismatch");
static_assert(sizeof(CModelInstance) == 0x30,
	"CModelInstance size mismatch");
static_assert(sizeof(CEffect) == 0x80, "CEffect size mismatch");
static_assert(sizeof(CRenderListNode) == 0x10,
              "CRenderListNode size mismatch");
static_assert(sizeof(ListHeader) == 0x18, "ListHeader size mismatch");
static_assert(sizeof(WorldObjectListEntry) == 0x18,
              "WorldObjectListEntry size mismatch");
static_assert(sizeof(WorldObjectEntry) == 0x24,
              "WorldObjectEntry size mismatch");
static_assert(sizeof(SceneNodeTintRecord) == 0x10,
              "SceneNodeTintRecord size mismatch");
static_assert(sizeof(GxStagePresetRecord) == 0x30,
              "GxStagePresetRecord size mismatch");
static_assert(sizeof(RenderOverridePresetChannelDataRecord) == 0x28,
              "RenderOverridePresetChannelDataRecord size mismatch");
static_assert(sizeof(RenderOverridePresetChannelResolverRecord) == 0x54,
              "RenderOverridePresetChannelResolverRecord size mismatch");
static_assert(sizeof(RenderOverrideTransformAuxRecord) == 0x50,
              "RenderOverrideTransformAuxRecord size mismatch");
static_assert(sizeof(RenderOverrideDependencyGateOutputRecord) == 0x10,
              "RenderOverrideDependencyGateOutputRecord size mismatch");
static_assert(sizeof(RenderOverrideCompactScalarOutputEntry) == 0x04,
              "RenderOverrideCompactScalarOutputEntry size mismatch");
static_assert(sizeof(CModelPartFrameWindowRecord) == 0x10,
              "CModelPartFrameWindowRecord size mismatch");
static_assert(sizeof(CModelPartSequenceFrameRecord) == 0x8C,
              "CModelPartSequenceFrameRecord size mismatch");
static_assert(sizeof(CModelPartStateDefRecord) == 0xE4,
              "CModelPartStateDefRecord size mismatch");
static_assert(sizeof(CModelPartWeightVisibilityRecord) == 0x38,
              "CModelPartWeightVisibilityRecord size mismatch");
static_assert(sizeof(CModelVisibilityDependencyRecord) == 0x1C,
              "CModelVisibilityDependencyRecord size mismatch");
static_assert(sizeof(ParticleEmitterOverrideSlot) == 0x80,
              "ParticleEmitterOverrideSlot size mismatch");
static_assert(sizeof(PlaneParticleEmitterOverrideSlot) == 0x8C,
              "PlaneParticleEmitterOverrideSlot size mismatch");
static_assert(sizeof(CGxuLight) == 0x2C, "CGxuLight size mismatch");
static_assert(sizeof(RenderOverrideLocalPointOutputRecord) == 0x40,
              "RenderOverrideLocalPointOutputRecord size mismatch");
static_assert(sizeof(GeosetPrimitiveRecord) == 0x08,
              "GeosetPrimitiveRecord size mismatch");
static_assert(sizeof(GeosetUvLayerVec2ArrayRecord) == 0x2C,
              "GeosetUvLayerVec2ArrayRecord size mismatch");
static_assert(sizeof(CModelGeosetBindingRecord) == 0x10,
              "CModelGeosetBindingRecord size mismatch");
static_assert(sizeof(CAnimRibbonObjStatus) == 0x74,
              "CAnimRibbonObjStatus size mismatch");
static_assert(sizeof(CParticleEmitterRuntimeParticle) == 0x28,
              "CParticleEmitterRuntimeParticle size mismatch");
static_assert(sizeof(CParticleEmitterRuntime) == 0x68,
              "CParticleEmitterRuntime size mismatch");
static_assert(sizeof(CAnimRibbonObj) == 0x16C,
              "CAnimRibbonObj size mismatch");
static_assert(sizeof(SceneNodeChildLink) == 0x10,
              "SceneNodeChildLink size mismatch");
static_assert(sizeof(SceneNodeChildBucket) == 0x0C,
              "SceneNodeChildBucket size mismatch");
static_assert(sizeof(RenderBatchElement) == 0x14,
              "RenderBatchElement size mismatch");
static_assert(sizeof(MeshLayerStateRecord) == 0x24,
              "MeshLayerStateRecord size mismatch");
static_assert(sizeof(MeshLayerDispatchRecord) == 0x2C,
              "MeshLayerDispatchRecord size mismatch");
static_assert(sizeof(MeshAuxResourceEntry) == 0x2C,
              "MeshAuxResourceEntry size mismatch");
static_assert(sizeof(LayerInfo) == 0x14, "LayerInfo size mismatch");
static_assert(sizeof(MeshData) == 0x128, "MeshData size mismatch");
static_assert(sizeof(CWorldFrameWar3) == 0x668,
              "CWorldFrameWar3 size mismatch");


struct CSimpleMessageFrame {
	void**						vf_table;				// 0x0
	uint32_t					ref_count;				// 0x4

	// wip
};

struct CInfoPanelUnitDetail;
struct CInfoPanelBuildingDetail;
struct CInfoPanelCargoDetail;
struct CInfoPanelGroup;
struct CInfoPanelItemDetail;
struct CInfoPanelDestructableDetail;
struct CInventoryBar;

struct CBuffBar;

struct CInventoryBar {
public:
	using node_t = struct _Node {
		byte_t			ukn_0x0[0x4];
		CCommandButton* button;
	};
public:
	void**							vf_table;				// 0x0
	unsigned char					ukn_0x4[0x124];
	GameSimpleArray<node_t>			buttons;				// 0x128
};

struct CInfoPanelUnitDetail {
	void**							vf_table;				// 0x0
	uint32_t						ref_count;				// 0x4
	unsigned char					ukn_0x8[0x164];
	CBuffBar*						buff_bar;				// 0x16C
};

struct CInfoBar {
	void**							vf_table;				// 0x0
	uint32_t						ref_count;				// 0x4
	unsigned char					ukn_0x8[0x124];
	void*							cur_frame;				// 0x12C
	CInfoPanelUnitDetail*			unit_detail;			// 0x130
	CInfoPanelBuildingDetail*		building_detail;		// 0x134
	CInfoPanelCargoDetail*			cargo_detail;			// 0x138
	CInfoPanelGroup*				group_detail;			// 0x13C
	CInfoPanelItemDetail*			item_detail;			// 0x140
	CInfoPanelDestructableDetail*	destructable_detail;	// 0x144
	CInventoryBar*					inventory_bar;			// 0x148
	CSimpleFrame*					inventory_cover;		// 0x14C
	CSimpleFontString*				inventory_text;			// 0x150
};

struct CCursorFrame;
struct CToolTipWar3;
struct CUberToolTipWar3;

struct CGameUI {
	void**						vf_table;				// 0x0
	unsigned char				ukn_0x4[0x28];
	CSpriteUber*				cursor_sprite;			// 0x2C
	unsigned char				ukn_0x30[0x13C];
	CCursorFrame*				cursor_frame;			// 0x16C
	unsigned char				ukn_0x170[0x44];
	void*						cur_mode;				// 0x1B4	当前mode
	uint32_t					max_mode_count;			// 0x1B8	最大mode数量
	uint32_t					cur_mode_count;			// 0x1BC	当前mode数量
	void**						cur_select_mode;		// 0x1C0	当前select_mode数组
	unsigned char				ukn_0x1C4[0x4];
	CToolTipWar3*				tool_tip;				// 0x1C8
	CUberToolTipWar3*			uber_tool_tip;			// 0x1CC
	unsigned char				ukn_0x1D0[0x40];
	CTargetMode*				target_mode;			// 0x210
	CSelectMode*				select_mode;			// 0x214
	CDragSelectMode*			drag_select_mode;		// 0x218
	CDragScrollMode*			drag_scroll_mode;		// 0x21C
	CBuildMode*					build_mode;				// 0x220
	CSignalMode*				signal_mode;			// 0x224
	CEscMenu*					esc_menu;				// 0x228
	unsigned char				ukn_0x22C[0x4];
	CSuspendIndicator*			suspend_indicator;		// 0x230
	CUnresponsiveIndicator*		unresponsive_indicator;	// 0x234
	CAllianceMode*				alliance_mode;			// 0x238
	CChatMode*					chat_mode;				// 0x23C
	CScriptDialogMode*			script_dialog_mode;		// 0x240
	CQuestMode*					quest_mode;				// 0x244
	unsigned char				ukn_0x248[0x4];
	CSelecetionManager*			selection_manager;		// 0x24C
	CDragScrollManager*			drag_scroll_manager;	// 0x250
	CCameraWar3*				camera_war3;			// 0x254
	unsigned char				ukn_0x258[0x154];
	CMultiBoard*				multi_board;			// 0x3AC
	unsigned char				ukn_0x3B0[0xC];
	CWorldFrameWar3*			world_frame_war3;		// 0x3BC
	CMiniMap*					mini_map;				// 0x3C0
	CInfoBar*					info_bar;				// 0x3C4
	CCommandBar*				command_bar;			// 0x3C8
	CResourceBar*				resource_bar;			// 0x3CC
	CUpperButtonBar*			upper_button_bar;		// 0x3D0
	CSimpleFrame*				ukn_frame_0x3D4;
	CSimpleButton*				command_bar_cover;		// 0x3D8 技能栏遮罩, 万恶之源
	CHeroBar*					hero_bar;				// 0x3DC
	CPeonBar*					peon_bar;				// 0x3E0
	CSimpleMessageFrame*		error_message;			// 0x3E4
	CSimpleMessageFrame*		text_message;			// 0x3E8
	CSimpleMessageFrame*		chat_message;			// 0x3EC
	CSimpleMessageFrame*		upkeep_message;			// 0x3F0
	CPortraitButton*			portrait_button;		// 0x3F4
	CTimeOfDayIndicator*		time_indicator;			// 0x3F8
	CChatEditBar*				chat_edit_bar;			// 0x3FC
	CCinematicPanel*			cinematice_panel;		// 0x400
	unsigned char				ukn_0x404[4];
	CSimpleButton*				button_minimap_signal;	// 0x408
	CSimpleButton*				button_minimap_terrain;	// 0x40C
	CSimpleButton*				button_minimap_color;	// 0x410
	CSimpleButton*				button_minimap_creep;	// 0x414
	CSimpleButton*				button_formation;		// 0x418
	CFrame*						ukn_frame_0x41C;
	CSimpleConsole*				ukn_frame_0x420;
	CFrame*						ukn_frame_0x424;
	CSimpleConsole*				console_ui;				// 0x428 ConsoleUI
	uint32_t					hotkey_quick_save;		// 0x42C
	uint32_t					hotkey_quick_load;		// 0x430
	uint32_t					hotkey_help;			// 0x434
	uint32_t					hotkey_option;			// 0x438
	uint32_t					hotkey_quit_game;		// 0x43C
	uint32_t					hotkey_minimap_signal;	// 0x440
	uint32_t					hotkey_minimap_terrain;	// 0x444
	uint32_t					hotkey_minimap_color;	// 0x448
	uint32_t					hotkey_minimap_creep;	// 0x44C
	uint32_t					hotkey_formation;		// 0x450
}; // sizeof = 0x454

struct CTargetMode {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	unsigned char		ukn_0x8[0x4];
	uint32_t			ability_id;				// 0xC
	uint32_t			order_id;				// 0x10
	uint32_t			cast_type;				// 0x14 释放类型
	HashGroup			lb_item;				// 0x18 左键物品
	HashGroup			rb_item;				// 0x20 右键物品
	HashGroup			source;					// 0x28 来源 selection->active_unit
	void*				target;					// 0x30
};

struct CSelectMode {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	unsigned char		ukn_0x8[0xC];
	void*				target;					// 0x14
};

struct CDragSelectMode {
	void**				vf_table;				// 0x0
};

struct CDragScrollMode {
	void**				vf_table;				// 0x0
};

struct CBuildMode {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	unsigned char		ukn_0x8[0x4];
	CBuildFrame*		build_frame;			// 0xC
};

struct CConstructUI;
struct CPlacementBox;

struct CBuildFrame {
	void**				vf_table;				// 0x0
	unsigned char		ukn_0x4[0x164];
	uint32_t			unit_id_0x168;			// 0x168
	unsigned char		ukn_0x16C[0x4];
	uint32_t			unit_id_0x170;			// 0x170
	HashGroup			item_hash;				// 0x174 象牙塔等
	float				x;						// 0x17C
	float				y;						// 0x180
	unsigned char		ukn_0x184[0x8];
	CConstructUI*		construct_ui;			// 0x18C
	CPlacementBox*		placement_box;			// 0x190
	CUnit*				builder;				// 0x194
	CAbility*			build_ability;			// 0x198
};

struct CSignalMode {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
};

// tbd
struct CEscMenu {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
};


struct CAllianceMode {
	void**				vf_table;				// 0x0
	byte_t				ukn_0x4[0x8];
	CAllianceDialog*	dialog;					// 0xC
};

struct CAllianceDialog {
	void**				vf_table;				// 0x0
	byte_t				ukn_0x4[0x164];
	CBackDropFrame*		backdrop;				// 0x168
	CAllianceSlot*		slots[12];				// 0x16C 12个玩家槽
};

struct CAllianceSlot {
	void**				vf_table;				// 0x0
	byte_t				ukn_0x4[0x180];
	CBackDropFrame*		backdrop;				// 0x184
	CTextFrame*			title;					// 0x188 玩家标题
	CGlueCheckBoxWar3*	alliance;				// 0x18C 联盟
	CGlueCheckBoxWar3*	share_vision;			// 0x190 共享视野
	CGlueCheckBoxWar3*	share_units;			// 0x194 共享单位
	CBackDropFrame*		gold_texture;			// 0x198 黄金贴图
	CBackDropFrame*		lumber_texture;			// 0x19C 木材贴图
	CTextFrame*			gold_text;				// 0x1A0 黄金文本
	CTextFrame*			lumber_text;			// 0x1A4 木材文本
};

// tbd
struct CCommandButton {
	CSimpleButton		simple_button;			// 0x0
	unsigned char		ukn_0x168[0x28];
	CCommandButtonData* button_data;			// 0x190
	CSimpleFrame*		subscript_parent;		// 0x194
	CSimpleFontString*	subscript;				// 0x198
	CSpriteFrame*		cooldown;				// 0x19C
	unsigned char		ukn_0x1A0[0x8];
	CSpriteFrame*		stream;					// 0x1A8
}; // sizeof = 0x1B8

struct CCommandButtonData {
	void**				vf_table;				// 0x0
	uint32_t			ability_id;				// 0x4	物品按钮和基本技能这里是0
	uint32_t			order_id[0x2];			// 0x8	左键/右键命令
	uint32_t			cast_type[0x2];			// 0x10 技能的cast_type
	uint32_t			hash_0x18;				// 0x18 与hash_0x20组成lb_item
	uint32_t			hash_0x1C;				// 0x1C 与hash_0x24组成rb_item
	uint32_t			hash_0x20;				// 0x20
	uint32_t			hash_0x24;				// 0x24
	uint32_t			tooltip_type;			// 0x28 0 - 不显示蓝耗, 1 - 显示蓝耗, 2 - 不显示tooltip
	char				tip[0x100];				// 0x2C
	unsigned char		ukn_0x12C[0x60];
	char				ubertip[0x400];			// 0x18C
	uint32_t			gold_cost;				// 0x58C
	uint32_t			lumber_cost;			// 0x590
	uint32_t			mana_cost;				// 0x594
	uint32_t			food_cost;				// 0x598
	uint32_t			x;						// 0x59C
	uint32_t			y;						// 0x5A0
	uint32_t			flag_0x5A4;				// 0x5A4
	uint32_t			flag_0x5A8;				// 0x5A8
	uint32_t			hotkey;					// 0x5AC
	uint32_t			show_highlight;			// 0x5B0 显示高亮
	uint32_t			flag_0x5B4;				// 0x5B4
	uint32_t			flag_0x5B8;				// 0x5B8
	BOOL				active;					// 0x5BC 按钮状态
	uint32_t			sub_script;				// 0x5C0 下标
	uint32_t			flag_0x5C4;				// 0x5C4
	char				art[0x104];				// 0x5C8
	unsigned char		ukn_0x6CC[0x8];
	CAbility*			ability;				// 0x6D4 基本技能(例如move)这里是NULL
}; // sizeof = 0x6D8

// 命令按钮栏
struct CCommandBar {
public:
	using buttons_t = GameArray2<GameArray2<CCommandButton*>>;
public:
	void**				vf_table;				// 0x0
	byte_t				ukn_0x4[0x148];
	buttons_t			buttons;				// 0x14C
}; // sizeof = 0x1C0



struct CBulletBase;
struct CBullet;
struct ProjectilePosition;


struct CWar3Image {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	unsigned char		ukn_0x8[0xC];
	CObserver			observer;				// 0x14
	uint32_t			flag_0x20;				// 0x20
	unsigned char		ukn_0x24[0x4];
	CSpriteUber*		sprite;					// 0x28
}; // sizeof = 0x2C

struct CBulletBase {
	CWar3Image			image;					// 0x0
	HashGroup			source_hash;			// 0x2C
	unsigned char		ukn_0x34[0x4];
	uint32_t			damage_type;			// 0x38
	CAbility*			ability;				// 0x3C
	FloatMini			base_damage;			// 0x40
	FloatMini			bonus_damage;			// 0x48
	FloatMini			speed;					// 0x50 射弹速度
	uint32_t			weapon_type;			// 0x58
	uint32_t			attack_type;			// 0x5C
	uint32_t			damage_flag;			// 0x60
	CAgentTimer			timer_0x64;				// 0x64
}; // sizeof = 0x78

struct ProjectilePosition {
	void**				vf_table;				// 0x0
	uint32_t			ref_count;				// 0x4
	HashGroup			pos_hash;				// 0x8
}; // sizeof = 0x10

struct CBullet {
	CBulletBase			bullet_base;			// 0x0
	ProjectilePosition	postion;				// 0x78
	FloatMini			arc;					// 0x88 射弹弧度
}; // sizeof = 0x90

struct CArtillery {
	CBullet				bullet;
	uint32_t			owner_id;				// 0x90 玩家id
	HashGroup			target_hash;			// 0x94
	unsigned int		unk_9C;					   
	SplashData			splash_data;			// 0xA4
};//sizeof = 0xCC

struct CMissile;
struct CMissileLine;
struct CMissileBounce;
struct CMissileSplash;

// 'B-Mi'
struct CMissile {
	CBullet				bullet;					// 0x0
	FloatMini			target_x;				// 0x90
	FloatMini			target_y;				// 0x98
	HashGroup			target_hash;			// 0xA0
	unsigned char		ukn_0xA8[0x4];
}; // sizeof = 0xAC

// 'B-Ml'
struct CMissileLine {
	CMissile			missile;				// 0x0
	uint32_t			attacker_id;			// 0xAC
	unsigned char		ukn_0xB0[0x8];
	FloatMini			dmg_loss;				// 0xB8 伤害衰减
	FloatMini			spill_range;			// 0xC0 穿透距离
	FloatMini			spill_radius;			// 0xC8 穿透半径
	FloatMini			angel;					// 0xD0 角度
	unsigned char		ukn_0xD4[0x3C];
}; // sizeof = 0x110

// 'B-Mb'
struct CMissileBounce {
	CMissile			missile;				// 0x0
	uint32_t			source_owner_id;		// 0xAC
	uint32_t			target_allow;			// 0xB0
	uint32_t			bounce_times;			// 0xB4 剩余弹射次数
	FloatMini			bounce_radius;			// 0xB8
	FloatMini			dmg_loss;				// 0xC0
}; // sizeof = 0xC8

// 'B-Ms'
struct CMissileSplash {
	CMissile			missile;				// 0x0
	uint32_t			source_owner_id;		// 0xAC
	SplashData			splash_data;			// 0xB4
}; // sizeof = 0xDC




struct CMissileThunderBoltTwo;
struct CMissileShockWave;

// 'MHt2'
struct CMissileThunderBoltTwo {
	CMissile			missile;				// 0x0
	FloatMini			dur;					// 0xAC
	unsigned char		ukn_0xB4[0x28];
	HashGroup			ability_hash;			// 0xDC
};

// 'MOsh'
struct CMissileShockWave {
	CBullet				bullet;					// 0x0
	FloatMini			target_x;				// 0x90
	FloatMini			target_y;				// 0x98
	FloatMini			distance;				// 0xA0 距离
	HashGroup			target_hash;			// 0xA8
	unsigned char		ukn_0xB0[0x4];
	uint32_t			target_allow;			// 0xB4
	HashGroup			source_hash;			// 0xB8
	unsigned char		ukn_0xC0[0x4];
	uint32_t			source_owner_id;		// 0xC4
	FloatMini			area;					// 0xC8 影响区域
	FloatMini			final_area;				// 0xD0 最终影响区域
	FloatMini			damage;					// 0xD8
	FloatMini			max_damage;				// 0xE0
	unsigned char		ukn_0xE4[0x8];
	FloatMini			float_mini_0xEC;		// 初始化3 每次刷新减少0.12
	unsigned char		ukn_0xF0[0x18];
	float				float_0x108;			// ?
};











