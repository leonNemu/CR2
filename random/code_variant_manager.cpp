#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>

#include "code_variant_manager.h"
#include "instr_generator.h"

CodeVariantManager::CVM_MAPS CodeVariantManager::_all_cvm_maps;
std::string CodeVariantManager::_code_variant_img_path;
SIZE CodeVariantManager::_cc_offset = 0;
SIZE CodeVariantManager::_ss_offset = 0;
P_ADDRX CodeVariantManager::_org_stack_load_base = 0;
P_ADDRX CodeVariantManager::_ss_load_base = 0;
S_ADDRX CodeVariantManager::_ss_base = 0;
P_SIZE CodeVariantManager::_ss_load_size = 0;
std::string CodeVariantManager::_ss_shm_path;
INT32 CodeVariantManager::_ss_fd = -1;
BOOL CodeVariantManager::_is_cv1_ready = false;
BOOL CodeVariantManager::_is_cv2_ready = false;

static std::string get_real_path(const char *file_path)
{
    #define PATH_LEN 1024
    #define INCREASE_IDX ++idx;idx%=2
    #define OTHER_IDX (idx==0 ? 1 : 0)
    #define CURR_IDX idx
    
    char path[2][PATH_LEN];
    for(INT32 i=0; i<2; i++)
        memset(path[i], '\0', PATH_LEN);
    INT32 idx = 0;
    INT32 ret = 0;
    struct stat statbuf;
    //init
    strcpy(path[CURR_IDX], file_path);
    //loop to find real path
    while(1){
        ret = lstat(path[CURR_IDX], &statbuf);
        if(ret!=0)//lstat failed
            break;
        if(S_ISLNK(statbuf.st_mode)){
            ret = readlink(path[CURR_IDX], path[OTHER_IDX], PATH_LEN);
            PERROR(ret>0, "readlink error!\n");
            INCREASE_IDX;
        }else
            break;
    }
    
    return std::string(path[CURR_IDX]); 
}

static std::string get_real_name_from_path(std::string path)
{
    std::string real_path = get_real_path(path.c_str());
    UINT32 found = real_path.find_last_of("/");
    std::string name;
    if(found==std::string::npos)
        name = real_path;
    else
        name = real_path.substr(found+1);

    return name;
}

CodeVariantManager::CodeVariantManager(std::string module_path)
{
    _elf_real_name = get_real_name_from_path(get_real_path(module_path.c_str()));
    add_cvm(this);
}

static INT32 map_shm_file(std::string shm_path, S_ADDRX &addr, F_SIZE &file_sz)
{
    //1.open shm file
    INT32 fd = shm_open(shm_path.c_str(), O_RDWR, 0644);
    ASSERTM(fd!=-1, "open shm file failed! %s", strerror(errno));
    //2.calculate file size
    struct stat statbuf;
    INT32 ret = fstat(fd, &statbuf);
    FATAL(ret!=0, "fstat failed %s!", strerror(errno));
    F_SIZE file_size = statbuf.st_size;
    //3.mmap file
    void *map_ret = mmap(NULL, file_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    PERROR(map_ret!=MAP_FAILED, "mmap failed!");

    //return 
    addr = (S_ADDRX)map_ret;
    file_sz = file_size;
    return fd;
}

static void close_shm_file(INT32 fd)
{
    close(fd);
}

CodeVariantManager::~CodeVariantManager()
{
    close_shm_file(_cc_fd);
    //close_shm_file(_ss_fd);
    munmap((void*)_cc1_base, _cc_load_size*2);
    //munmap((void*)_ss_base, _ss_load_size);
}

void CodeVariantManager::init_cc()
{
    // 1.map cc
    S_SIZE map_size = 0;
    _cc_fd = map_shm_file(_cc_shm_path, _cc1_base, map_size);
    ASSERT(map_size==(_cc_load_size*2));
    _cc2_base = _cc1_base+map_size/2;  
}

void CodeVariantManager::init_cc_and_ss()
{
    // map cc
    for(CVM_MAPS::iterator iter = _all_cvm_maps.begin(); iter!=_all_cvm_maps.end(); iter++)
        iter->second->init_cc();
    // map ss
    S_SIZE map_size;
    S_ADDRX map_start;
    _ss_fd = map_shm_file(_ss_shm_path, map_start, map_size);
    _ss_base = map_start + map_size;
    ASSERT(map_size==_ss_load_size);
}

typedef struct mapsRow{
	P_ADDRX start;
	P_ADDRX end;
	SIZE offset;
	char perms[8];
	char dev[8];
	INT32 inode;
	char pathname[128];
}MapsFileItem;

inline BOOL is_executable(const MapsFileItem *item_ptr)
{
	if(strstr(item_ptr->perms, "x"))
		return true;
	else
		return false;
}

inline BOOL is_shared(const MapsFileItem *item_ptr)
{
	if(strstr(item_ptr->perms, "s"))
		return true;
	else
		return false;
}

void CodeVariantManager::parse_proc_maps(PID protected_pid)
{
    //1.open maps file
    char maps_path[100];
    sprintf(maps_path, "/proc/%d/maps", protected_pid);
    FILE *maps_file = fopen(maps_path, "r"); 
    ASSERTM(maps_file!=NULL, "Open %s failed!\n", maps_path);
    //2.find strings
    char shared_prefix_str[256];
    sprintf(shared_prefix_str, "%d-", protected_pid);
    std::string shared_prefix = std::string(shared_prefix_str);
    std::string cc_sufix = ".cc";
    std::string ss_sufix = ".ss";
    //3.read maps file, line by line
    #define mapsRowMaxNum 100
    MapsFileItem mapsArray[mapsRowMaxNum] = {{0}};
    INT32 mapsRowNum = 0;
    char row_buffer[256];
    char *ret = fgets(row_buffer, 256, maps_file);
    FATAL(!ret, "fgets wrong!\n");
    while(!feof(maps_file)){
        MapsFileItem *currentRow = mapsArray+mapsRowNum;
        //read one row
        currentRow->pathname[0] = '\0';
        sscanf(row_buffer, "%lx-%lx %s %lx %s %d %s", &(currentRow->start), &(currentRow->end), \
            currentRow->perms, &(currentRow->offset), currentRow->dev, &(currentRow->inode), currentRow->pathname);
        //x and code cache
        if(is_executable(currentRow) && !strstr(currentRow->pathname, "[vdso]") && !strstr(currentRow->pathname, "[vsyscall]")){
            std::string maps_record_name = get_real_name_from_path(get_real_path(currentRow->pathname));
            if(is_shared(currentRow)){
                SIZE prefix_pos = maps_record_name.find(shared_prefix);
                ASSERT(prefix_pos!=std::string::npos);
                prefix_pos += shared_prefix.length();
                SIZE cc_pos = maps_record_name.find(cc_sufix);
                ASSERT(cc_pos!=std::string::npos);
                //find cvm
                CVM_MAPS::iterator iter = _all_cvm_maps.find(maps_record_name.substr(prefix_pos, cc_pos-prefix_pos));
                ASSERT(iter!=_all_cvm_maps.end());
                iter->second->set_cc_load_info(currentRow->start, currentRow->end-currentRow->start, maps_record_name);
            }else{
                CVM_MAPS::iterator iter = _all_cvm_maps.find(maps_record_name);
                ASSERT(iter!=_all_cvm_maps.end());
                iter->second->set_x_load_base(currentRow->start, currentRow->end - currentRow->start);
            }
        }
        //shadow stack and stack
        if(!is_executable(currentRow)){
            std::string maps_record_name = get_real_name_from_path(get_real_path(currentRow->pathname));
            if(is_shared(currentRow)){//shadow stack
                ASSERT(maps_record_name.find(ss_sufix)!=std::string::npos);
                set_ss_load_info(currentRow->end, currentRow->end-currentRow->start, maps_record_name);
            }else{
                if(strstr(currentRow->pathname, "[stack]"))//stack
                    set_stack_load_base(currentRow->end);
            }
        }
        
        //calculate the row number
        mapsRowNum++;
        ASSERT(mapsRowNum < mapsRowMaxNum);
        //read next row
        ret = fgets(row_buffer, 256, maps_file);
    }

    fclose(maps_file);
    return ;
}

#define JMP32_LEN 0x5
#define JMP8_LEN 0x2
#define OVERLAP_JMP32_LEN 0x4
#define OFFSET_POS 0x1
#define JMP8_OPCODE 0xeb
#define JMP32_OPCODE 0xe9

inline CC_LAYOUT_PAIR place_invalid_boundary(S_ADDRX invalid_addr, CC_LAYOUT &cc_layout)
{
    std::string invalid_template = InstrGenerator::gen_invalid_instr();
    invalid_template.copy((char*)invalid_addr, invalid_template.length());
    return cc_layout.insert(std::make_pair(Range<S_ADDRX>(invalid_addr, invalid_addr+invalid_template.length()-1), BOUNDARY_PTR));
}

inline CC_LAYOUT_PAIR place_invalid_trampoline(S_ADDRX invalid_addr, CC_LAYOUT &cc_layout)
{
    std::string invalid_template = InstrGenerator::gen_invalid_instr();
    invalid_template.copy((char*)invalid_addr, invalid_template.length());
    return cc_layout.insert(std::make_pair(Range<S_ADDRX>(invalid_addr, invalid_addr+invalid_template.length()-1), INV_TRAMP_PTR));
}

inline CC_LAYOUT_PAIR place_trampoline8(S_ADDRX tramp8_addr, INT8 offset8, CC_LAYOUT &cc_layout)
{
    //gen jmp rel8 instruction
    UINT16 pos = 0;
    std::string jmp_rel8_template = InstrGenerator::gen_jump_rel8_instr(pos, offset8);
    jmp_rel8_template.copy((char*)tramp8_addr, jmp_rel8_template.length());
    //insert trampoline8 into cc layout
    return cc_layout.insert(std::make_pair(Range<S_ADDRX>(tramp8_addr, tramp8_addr+JMP8_LEN-1), TRAMP_JMP8_PTR));
}
inline CC_LAYOUT_PAIR place_trampoline32(S_ADDRX tramp32_addr, INT32 offset32, CC_LAYOUT &cc_layout)
{
    //gen jmp rel32 instruction
    UINT16 pos = 0;
    std::string jmp_rel32_template = InstrGenerator::gen_jump_rel32_instr(pos, offset32);
    jmp_rel32_template.copy((char*)tramp32_addr, jmp_rel32_template.length());
    //insert trampoline8 into cc layout
    return cc_layout.insert(std::make_pair(Range<S_ADDRX>(tramp32_addr, tramp32_addr+JMP32_LEN-1), TRAMP_JMP32_PTR));
}

inline CC_LAYOUT_PAIR place_overlap_trampoline32(S_ADDRX tramp32_addr, INT32 offset32, CC_LAYOUT &cc_layout)
{
    //gen jmp rel32 instruction
    UINT16 pos = 0;
    std::string jmp_rel32_template = InstrGenerator::gen_jump_rel32_instr(pos, offset32);
    jmp_rel32_template.copy((char*)tramp32_addr, jmp_rel32_template.length());
    //insert trampoline8 into cc layout
    return cc_layout.insert(std::make_pair(Range<S_ADDRX>(tramp32_addr, tramp32_addr+OVERLAP_JMP32_LEN-1), TRAMP_OVERLAP_JMP32_PTR));
}

//this function is only used to place trampoline8 with trampoline32 
S_ADDRX front_to_place_overlap_trampoline32(S_ADDRX overlap_tramp_addr, UINT8 overlap_byte, CC_LAYOUT &cc_layout)
{
    // 1. set boundary to start searching
    std::pair<CC_LAYOUT_ITER, BOOL> boundary = cc_layout.insert(std::make_pair(Range<S_ADDRX>(overlap_tramp_addr, \
        overlap_tramp_addr+OVERLAP_JMP32_LEN-1), TRAMP_OVERLAP_JMP32_PTR));
    CC_LAYOUT_ITER curr_iter = boundary.first, prev_iter = --boundary.first;
    ASSERT(curr_iter!=cc_layout.begin());
    S_ADDRX trampoline32_addr = 0;
    //loop to find the space to place the tramp32
    while(prev_iter!=cc_layout.end()){
        S_SIZE left_space = curr_iter->first - prev_iter->first;
        if(left_space>=JMP32_LEN){
            trampoline32_addr = curr_iter->first.low() - JMP32_LEN;
            S_ADDRX end = prev_iter->first.high() + 1;
            while(trampoline32_addr!=end){
                INT32 offset32 = trampoline32_addr - overlap_tramp_addr - JMP32_LEN;
                if(((offset32>>24)&0xff)==overlap_byte){
                    CC_LAYOUT_PAIR ret = place_overlap_trampoline32(overlap_tramp_addr, offset32, cc_layout);
                    FATAL(ret.second, "place overlap trampoline32 wrong!\n");
                    return trampoline32_addr;
                }
                trampoline32_addr--;
            }
        }
    
        curr_iter--;
        prev_iter--;
    }
    ASSERT(prev_iter!=cc_layout.end());
    return trampoline32_addr;
}

S_ADDRX front_to_place_trampoline32(S_ADDRX fixed_trampoline_addr, CC_LAYOUT &cc_layout)
{
    // 1. set boundary to start searching
    std::pair<CC_LAYOUT_ITER, BOOL> boundary = cc_layout.insert(std::make_pair(Range<S_ADDRX>(fixed_trampoline_addr, \
        fixed_trampoline_addr+JMP8_LEN-1), TRAMP_JMP8_PTR));
    ASSERT(boundary.second);
    // 2. init scanner
    CC_LAYOUT_ITER curr_iter = boundary.first, prev_iter = --boundary.first;
    ASSERT(curr_iter!=cc_layout.begin());
    // 3. variables used to store the results
    S_ADDRX trampoline32_addr = 0;
    S_ADDRX trampoline8_base = fixed_trampoline_addr;
    S_ADDRX last_tramp8_addr = 0;
    // 4. loop to search
    while(prev_iter!=cc_layout.begin()){
        S_SIZE space = curr_iter->first - prev_iter->first;
        ASSERT(space>=0);
        // assumpe the dest can place tramp32
        trampoline32_addr = curr_iter->first.low() - JMP32_LEN;
        INT32 dest_offset8 = trampoline32_addr - trampoline8_base - JMP8_LEN;
        // 4.1 judge can place trampoline32
        if((space>=JMP32_LEN) && (dest_offset8>=SCHAR_MIN)){
            CC_LAYOUT_PAIR ret = place_trampoline8(trampoline8_base, dest_offset8, cc_layout);
            FATAL((trampoline8_base!=fixed_trampoline_addr) && !ret.second, " place trampoline8 wrong!\n");
            break;
        }
        // assumpe the dest can place tramp8
        S_ADDRX tramp8_addr = curr_iter->first.low() - JMP8_LEN;
        INT32 relay_offset8 = tramp8_addr - trampoline8_base - JMP8_LEN;
        // 4.2 judge is over 8 relative offset
        if(relay_offset8>=SCHAR_MIN)
            last_tramp8_addr = space>=JMP8_LEN ? tramp8_addr : last_tramp8_addr;
        else{//need relay
            if(last_tramp8_addr==0){
                CC_LAYOUT_ITER erase_iter = cc_layout.upper_bound(Range<S_ADDRX>(fixed_trampoline_addr));
                cc_layout.erase(--erase_iter);
                return 0;
            }
            INT32 last_offset8 = last_tramp8_addr - trampoline8_base - JMP8_LEN;
            ASSERT(last_offset8>=SCHAR_MIN);
            //place the internal jmp8 rel8 template
            CC_LAYOUT_PAIR ret = place_trampoline8(trampoline8_base, last_offset8, cc_layout);
            FATAL((trampoline8_base!=fixed_trampoline_addr) && !ret.second, " place trampoline8 wrong!\n");
            //clear last
            trampoline8_base = last_tramp8_addr;
            last_tramp8_addr = 0;
        }

        curr_iter--;
        prev_iter--;
    }
    ASSERT(prev_iter!=cc_layout.begin());
    ASSERT(trampoline32_addr!=0);
    return trampoline32_addr;
}

S_ADDRX *random(const CodeVariantManager::RAND_BBL_MAPS fixed_rbbls, const CodeVariantManager::RAND_BBL_MAPS movable_rbbls, \
    SIZE &array_num)
{
    array_num = fixed_rbbls.size() + movable_rbbls.size();
    S_ADDRX *rbbl_array = new S_ADDRX[array_num];
    //init array
    SIZE index = 0;
    for(CodeVariantManager::RAND_BBL_MAPS::const_iterator iter = fixed_rbbls.begin(); iter!=fixed_rbbls.end(); iter++, index++)
        rbbl_array[index] = (S_ADDRX)iter->second;
    for(CodeVariantManager::RAND_BBL_MAPS::const_iterator iter = movable_rbbls.begin(); iter!=movable_rbbls.end(); iter++, index++)
        rbbl_array[index] = (S_ADDRX)iter->second;
    //random seed
    srand((INT32)time(NULL));
    for(SIZE idx=array_num-1; idx>0; idx--){
        //swap
        INT32 swap_idx = rand()%idx;
        INT32 temp = rbbl_array[swap_idx];
        rbbl_array[swap_idx] = rbbl_array[idx];
        rbbl_array[idx] = temp;
    }

    return rbbl_array;
}

S_ADDRX CodeVariantManager::arrange_cc_layout(S_ADDRX cc_base, CC_LAYOUT &cc_layout, \
    RBBL_CC_MAPS &rbbl_maps, JMPIN_CC_OFFSET &jmpin_rbbl_offsets)
{
    S_ADDRX trampoline32_addr = 0;
    S_ADDRX used_cc_base = 0;
    // 1.place fixed rbbl's trampoline  
    CC_LAYOUT_PAIR invalid_ret = place_invalid_boundary(cc_base, cc_layout);
    FATAL(!invalid_ret.second, " place invalid boundary wrong!\n");
    for(RAND_BBL_MAPS::iterator iter = _postion_fixed_rbbl_maps.begin(); iter!=_postion_fixed_rbbl_maps.end(); iter++){
        RAND_BBL_MAPS::iterator iter_bk = iter;
        F_SIZE curr_bbl_offset = iter->first;
        F_SIZE next_bbl_offset = (++iter)!=_postion_fixed_rbbl_maps.end() ? iter->first : curr_bbl_offset+JMP32_LEN;
        S_SIZE left_space = next_bbl_offset - curr_bbl_offset;
        iter = iter_bk;
        S_ADDRX inv_trampoline_addr = 0;
        //place trampoline
        if(left_space>=JMP32_LEN){
            trampoline32_addr = curr_bbl_offset + cc_base;
            used_cc_base = trampoline32_addr + JMP32_LEN;
        }else{
            if(left_space<JMP8_LEN){
                ERR("There is no space to place the trampoline (%lx), so we only place the invalid instruction!\n", curr_bbl_offset);
                inv_trampoline_addr = curr_bbl_offset + cc_base;
            }else{
                //search lower address to find space to place the jmp rel32 trampoline
                trampoline32_addr = front_to_place_trampoline32(curr_bbl_offset + cc_base, cc_layout);
                if(trampoline32_addr==0){
                    ERR("There is no space to place the trampoline (%lx), so we only place the invalid instruction!\n", curr_bbl_offset);
                    inv_trampoline_addr = curr_bbl_offset + cc_base;
                }
            }
            
            used_cc_base = next_bbl_offset + cc_base;
        }
        CC_LAYOUT_PAIR ret;
        if(inv_trampoline_addr!=0){
            //place invalid instr
            ret = place_invalid_trampoline(inv_trampoline_addr, cc_layout);
            FATAL(!ret.second, " place inv_trampoline wrong!\n");
            inv_trampoline_addr = 0;
        }else{
            //place tramp32
            ret = place_trampoline32(trampoline32_addr, curr_bbl_offset, cc_layout);
            FATAL(!ret.second, " place trampoline32 wrong!\n");
        }
    }
    // 2.place switch-case trampolines
    #define TRAMP_GAP 0x100
    S_ADDRX new_cc_base = used_cc_base + TRAMP_GAP;
    // get full jmpin targets
    TARGET_SET merge_set;
    for(JMPIN_ITERATOR iter = _switch_case_jmpin_rbbl_maps.begin(); iter!= _switch_case_jmpin_rbbl_maps.end(); iter++){
        F_SIZE jmpin_rbbl_offset = iter->first;
        P_SIZE jmpin_target_offset = _cc_offset + new_cc_base - cc_base;
        jmpin_rbbl_offsets.insert(std::make_pair(jmpin_rbbl_offset, jmpin_target_offset));
        merge_set.insert(iter->second.begin(), iter->second.end());
    }
    // 3.place jmpin targets trampoline
    for(TARGET_ITERATOR it = merge_set.begin(); it!=merge_set.end(); it++){
        TARGET_ITERATOR it_bk = it;
        F_SIZE curr_bbl_offset = *it;
        F_SIZE next_bbl_offset = (++it)!=merge_set.end() ? *it : curr_bbl_offset+JMP32_LEN;
        S_SIZE left_space = next_bbl_offset - curr_bbl_offset;
        it = it_bk;
        S_ADDRX inv_trampoline_addr = 0;
        //can place trampoline
        if(left_space>=JMP32_LEN){
            trampoline32_addr = curr_bbl_offset + new_cc_base;
            used_cc_base = trampoline32_addr + JMP32_LEN;
        }else{
            if(left_space<JMP8_LEN){
                ERR("There is no space to place the trampoline (%lx), so we only place the invalid instruction!\n", curr_bbl_offset);
                inv_trampoline_addr = curr_bbl_offset + new_cc_base;
            }else{
                //search lower address to find space to place the jmp rel32 trampoline
                trampoline32_addr = front_to_place_trampoline32(curr_bbl_offset + new_cc_base, cc_layout);
                if(trampoline32_addr==0){
                    ERR("There is no space to place the trampoline (%lx), so we only place the invalid instruction!\n", curr_bbl_offset);
                    inv_trampoline_addr = curr_bbl_offset + new_cc_base;
                }
            }
            
            used_cc_base = next_bbl_offset + new_cc_base;
        }

        CC_LAYOUT_PAIR ret;
        if(inv_trampoline_addr!=0){
            //place invalid instr
            ret = place_invalid_trampoline(inv_trampoline_addr, cc_layout);
            FATAL(!ret.second, " place inv_trampoline wrong!\n");
            inv_trampoline_addr = 0;
        }else{
            //place tramp32
            ret = place_trampoline32(trampoline32_addr, curr_bbl_offset, cc_layout);
            FATAL(!ret.second, " place trampoline32 wrong!\n");
        }
    }
    // 4.place fixed and movable rbbls
    // 4.1 random fixed and movable rbbls
    SIZE random_array_size;
    S_ADDRX *random_array = random(_postion_fixed_rbbl_maps, _movable_rbbl_maps, random_array_size);
    // 4.2 place rbbls
    for(SIZE idx = 0; idx<random_array_size; idx++){
        RandomBBL *rbbl = (RandomBBL*)random_array[idx];
        S_SIZE rbbl_size = rbbl->get_template_size();
        F_SIZE rbbl_offset = rbbl->get_rbbl_offset();
        rbbl_maps.insert(std::make_pair(rbbl_offset, used_cc_base));
        if(rbbl->has_lock_and_repeat_prefix()){//consider prefix
#ifdef TRACE_DEBUG
            rbbl_maps.insert(std::make_pair(rbbl_offset+1, used_cc_base+23));
#else
            rbbl_maps.insert(std::make_pair(rbbl_offset+1, used_cc_base+1));
#endif
        }
        cc_layout.insert(std::make_pair(Range<S_ADDRX>(used_cc_base, used_cc_base+rbbl_size-1), (S_ADDRX)rbbl));
        used_cc_base += rbbl_size;
    }
    // 4.3 free array
    delete []random_array;
#if 0    
    for(RAND_BBL_MAPS::iterator iter = _postion_fixed_rbbl_maps.begin(); iter!=_postion_fixed_rbbl_maps.end(); iter++){
        RandomBBL *rbbl = iter->second;
        S_SIZE rbbl_size = rbbl->get_template_size();
        rbbl_maps.insert(std::make_pair(iter->first, used_cc_base));
        if(rbbl->has_lock_and_repeat_prefix()){//consider prefix
#ifdef TRACE_DEBUG
            rbbl_maps.insert(std::make_pair(iter->first+1, used_cc_base+23));
#else
            rbbl_maps.insert(std::make_pair(iter->first+1, used_cc_base+1));
#endif
        }
        cc_layout.insert(std::make_pair(Range<S_ADDRX>(used_cc_base, used_cc_base+rbbl_size-1), (S_ADDRX)rbbl));
        used_cc_base += rbbl_size;
    }

    for(RAND_BBL_MAPS::iterator iter = _movable_rbbl_maps.begin(); iter!=_movable_rbbl_maps.end(); iter++){
        RandomBBL *rbbl = iter->second;
        S_SIZE rbbl_size = rbbl->get_template_size();
        rbbl_maps.insert(std::make_pair(iter->first, used_cc_base));
        if(rbbl->has_lock_and_repeat_prefix()){//consider prefix
#ifdef TRACE_DEBUG
            rbbl_maps.insert(std::make_pair(iter->first+1, used_cc_base+23));
#else
            rbbl_maps.insert(std::make_pair(iter->first+1, used_cc_base+1));
#endif            
        }
        cc_layout.insert(std::make_pair(Range<S_ADDRX>(used_cc_base, used_cc_base+rbbl_size-1), (S_ADDRX)rbbl));
        used_cc_base += rbbl_size;
    }
#endif
    //judge used cc size
    ASSERT((used_cc_base - cc_base)<=_cc_load_size);
    return used_cc_base;
}

void CodeVariantManager::relocate_rbbls_and_tramps(CC_LAYOUT &cc_layout, S_ADDRX cc_base, \
    RBBL_CC_MAPS &rbbl_maps, JMPIN_CC_OFFSET &jmpin_rbbl_offsets)
{
    for(CC_LAYOUT::iterator iter = cc_layout.begin(); iter!=cc_layout.end(); iter++){
        S_ADDRX range_base_addr = iter->first.low();
        S_SIZE range_size = iter->first.high() - range_base_addr + 1;
        switch(iter->second){
            case BOUNDARY_PTR: break;
            case INV_TRAMP_PTR: break;
            case TRAMP_JMP8_PTR: ASSERT(range_size>=JMP8_LEN); break;//has already relocated, when generate the jmp rel8
            case TRAMP_OVERLAP_JMP32_PTR: ASSERT(0); break;
            case TRAMP_JMP32_PTR://need relocate the trampolines
                {
                    ASSERT(range_size>=JMP32_LEN);
                    S_ADDRX relocate_addr = range_base_addr + 0x1;//opcode
                    S_ADDRX curr_pc = range_base_addr + JMP32_LEN;
                    F_SIZE target_rbbl_offset = (F_SIZE)(*(INT32*)relocate_addr);
                    RBBL_CC_MAPS::iterator ret = rbbl_maps.find(target_rbbl_offset);
                    ASSERT(ret!=rbbl_maps.end());
                    S_ADDRX target_rbbl_addr = ret->second;
                    INT64 offset64 = target_rbbl_addr - curr_pc;
                    ASSERT((offset64 > 0 ? offset64 : -offset64) < 0x7fffffff);
                    *(INT32*)relocate_addr = (INT32)offset64;
                }
                break;
            default://rbbl
                {
                    RandomBBL *rbbl = (RandomBBL*)iter->second;
                    F_SIZE rbbl_offset = rbbl->get_rbbl_offset();
                    JMPIN_CC_OFFSET::iterator ret = jmpin_rbbl_offsets.find(rbbl_offset);
                    P_SIZE jmpin_offset = ret!=jmpin_rbbl_offsets.end() ? ret->second : 0;//judge is switch case or not
                    rbbl->gen_code(cc_base, range_base_addr, range_size, _org_x_load_base, _cc_offset, _ss_offset, \
                        rbbl_maps, jmpin_offset);
                }
        }
    }
}

void CodeVariantManager::clean_cc(BOOL is_first_cc)
{
    S_ADDRX cc_base = is_first_cc ? _cc1_base : _cc2_base;
    S_ADDRX place_addr = cc_base;
    std::string invalid_instr = InstrGenerator::gen_invalid_instr();
    S_SIZE instr_len = invalid_instr.length();
    S_ADDRX cc_end = cc_base + _cc_load_size - instr_len;

    while(place_addr<=cc_end){
        invalid_instr.copy((char*)place_addr, instr_len);
        place_addr += instr_len;
    }
    return ;
}

void CodeVariantManager::generate_code_variant(BOOL is_first_cc)
{
    S_ADDRX cc_base = is_first_cc ? _cc1_base : _cc2_base;
    CC_LAYOUT &cc_layout = is_first_cc ? _cc_layout1 : _cc_layout2;
    RBBL_CC_MAPS &rbbl_maps = is_first_cc ? _rbbl_maps1 : _rbbl_maps2;
    JMPIN_CC_OFFSET &jmpin_rbbl_offsets = is_first_cc ? _jmpin_rbbl_offsets1 : _jmpin_rbbl_offsets2;
    // 1.clean code cache
    clean_cc(is_first_cc);
    // 2.arrange the code layout
    arrange_cc_layout(cc_base, cc_layout, rbbl_maps, jmpin_rbbl_offsets);
    // 3.generate the code
    relocate_rbbls_and_tramps(cc_layout, cc_base, rbbl_maps, jmpin_rbbl_offsets);

    return ;
}

void CodeVariantManager::generate_all_code_variant(BOOL is_first_cc)
{
    for(CVM_MAPS::iterator iter = _all_cvm_maps.begin(); iter!=_all_cvm_maps.end(); iter++)
        iter->second->generate_code_variant(is_first_cc);
    
    return ;
}

static BOOL need_stop = false;
pthread_t child_thread;

void CodeVariantManager::start_gen_code_variants()
{
    need_stop = false;
    pthread_create(&child_thread, NULL, generate_code_variant_concurrently, NULL);
}

void CodeVariantManager::stop_gen_code_variants()
{
    need_stop = true;
    pthread_join(child_thread, NULL);
}

void* CodeVariantManager::generate_code_variant_concurrently(void *arg)
{
    while(!need_stop){
        if(!_is_cv1_ready){
            generate_all_code_variant(true);
            _is_cv1_ready = true;
        }

        if(!_is_cv2_ready){
            generate_all_code_variant(false);
            _is_cv2_ready = true;
        }
        sched_yield();
    }
    return NULL;
}

void CodeVariantManager::clear_cv(BOOL is_first_cc)
{
    CC_LAYOUT &cc_layout = is_first_cc ? _cc_layout1 : _cc_layout2;
    RBBL_CC_MAPS &rbbl_maps = is_first_cc ? _rbbl_maps1 : _rbbl_maps2;
    JMPIN_CC_OFFSET &jmpin_rbbl_offsets = is_first_cc ? _jmpin_rbbl_offsets1 : _jmpin_rbbl_offsets2;
    cc_layout.clear();
    rbbl_maps.clear();
    jmpin_rbbl_offsets.clear();
}

void CodeVariantManager::clear_all_cv(BOOL is_first_cc)
{
    for(CVM_MAPS::iterator iter = _all_cvm_maps.begin(); iter!=_all_cvm_maps.end(); iter++)
        iter->second->clear_cv(is_first_cc);
}

void CodeVariantManager::consume_cv(BOOL is_first_cc)
{
    // 1.clear all code variants
    clear_all_cv(is_first_cc);
    // 2.clear ready flags
    if(is_first_cc){
        ASSERT(_is_cv1_ready);
        _is_cv1_ready = false;
    }else{
        ASSERT(_is_cv2_ready);
        
        _is_cv2_ready = false;
    }
}

void CodeVariantManager::wait_for_code_variant_ready(BOOL is_first_cc)
{
    BOOL &is_ready = is_first_cc ? _is_cv1_ready : _is_cv2_ready;
    while(!is_ready)
        sched_yield();
}

RandomBBL *CodeVariantManager::find_rbbl_from_all_paddrx(P_ADDRX p_addr, BOOL is_first_cc)
{
    ASSERT(is_first_cc ? _is_cv1_ready : _is_cv2_ready);
    
    for(CVM_MAPS::iterator iter = _all_cvm_maps.begin(); iter!=_all_cvm_maps.end(); iter++){
        RandomBBL *rbbl = iter->second->find_rbbl_from_paddrx(p_addr, is_first_cc);
        if(rbbl)
            return rbbl;
    }
    return NULL;
}

RandomBBL *CodeVariantManager::find_rbbl_from_all_saddrx(S_ADDRX s_addr, BOOL is_first_cc)
{
    ASSERT(is_first_cc ? _is_cv1_ready : _is_cv2_ready);
    
    for(CVM_MAPS::iterator iter = _all_cvm_maps.begin(); iter!=_all_cvm_maps.end(); iter++){
        RandomBBL *rbbl = iter->second->find_rbbl_from_saddrx(s_addr, is_first_cc);
        if(rbbl)
            return rbbl;
    }
    return NULL;
}

RandomBBL *CodeVariantManager::find_rbbl_from_paddrx(P_ADDRX p_addr, BOOL is_first_cc)
{
    S_ADDRX cc_base = is_first_cc ? _cc1_base : _cc2_base;
    
    if(p_addr>=_cc_load_base && p_addr<(_cc_load_base+_cc_load_size))
        return find_rbbl_from_saddrx(p_addr - _cc_load_base + cc_base, is_first_cc);
    else
        return NULL;
}

RandomBBL *CodeVariantManager::find_rbbl_from_saddrx(S_ADDRX s_addr, BOOL is_first_cc)
{
    S_ADDRX cc_base = is_first_cc ? _cc1_base : _cc2_base;
    CC_LAYOUT &cc_layout = is_first_cc ? _cc_layout1 : _cc_layout2;

    if(s_addr>=cc_base && s_addr<(cc_base+_cc_load_size)){
        CC_LAYOUT_ITER iter = cc_layout.upper_bound(Range<S_ADDRX>(s_addr));
        if(iter!=cc_layout.end() && s_addr<=(--iter)->first.high() && s_addr>=iter->first.low()){
            S_ADDRX ptr = iter->second;
            switch(ptr){
                case BOUNDARY_PTR: return NULL;
                case TRAMP_JMP8_PTR: 
                    {
                        ASSERT(*(UINT8*)ptr==JMP8_OPCODE);    
                        INT8 offset8 = *(INT8*)(ptr+OFFSET_POS);
                        S_ADDRX target_addr = s_addr + JMP8_LEN + offset8;    
                        return find_rbbl_from_saddrx(target_addr, is_first_cc);
                    }
                case TRAMP_OVERLAP_JMP32_PTR: ASSERT(0); return NULL;
                case TRAMP_JMP32_PTR://need relocate the trampolines
                    {
                        ASSERT(*(UINT8*)ptr==JMP32_OPCODE);    
                        INT32 offset32 = *(INT32*)(ptr+OFFSET_POS);
                        S_ADDRX target_addr = s_addr + JMP32_LEN + offset32;
                        return find_rbbl_from_saddrx(target_addr, is_first_cc);
                    }
                default://rbbl
                    return (RandomBBL*)ptr;
                }
        }else
            return NULL;
    }else
        return NULL;
}

P_ADDRX CodeVariantManager::find_cc_paddrx_from_all_orig(P_ADDRX orig_p_addrx, BOOL is_first_cc)
{
    ASSERT(is_first_cc ? _is_cv1_ready : _is_cv2_ready);

    for(CVM_MAPS::iterator iter = _all_cvm_maps.begin(); iter!=_all_cvm_maps.end(); iter++){
        P_ADDRX ret_addrx = iter->second->find_cc_paddrx_from_orig(orig_p_addrx, is_first_cc);
        if(ret_addrx!=0)
            return ret_addrx;
    }
    return 0;
}

S_ADDRX CodeVariantManager::find_cc_saddrx_from_all_orig(P_ADDRX orig_p_addrx, BOOL is_first_cc)
{
    ASSERT(is_first_cc ? _is_cv1_ready : _is_cv2_ready);

    for(CVM_MAPS::iterator iter = _all_cvm_maps.begin(); iter!=_all_cvm_maps.end(); iter++){
        P_ADDRX ret_addrx = iter->second->find_cc_saddrx_from_orig(orig_p_addrx, is_first_cc);
        if(ret_addrx!=0)
            return ret_addrx;
    }
    return 0;
}

S_ADDRX CodeVariantManager::find_cc_saddrx_from_orig(P_ADDRX orig_p_addrx, BOOL is_first_cc)
{
    RBBL_CC_MAPS &rbbl_maps = is_first_cc ? _rbbl_maps1 : _rbbl_maps2;
    F_SIZE pc_size = orig_p_addrx - _org_x_load_base;
    if(pc_size<_org_x_load_size){
        RBBL_CC_MAPS::iterator iter = rbbl_maps.find(pc_size);
        return iter!=rbbl_maps.end() ? iter->second : 0;
    }else
        return 0;
}

P_ADDRX CodeVariantManager::find_cc_paddrx_from_orig(P_ADDRX orig_p_addrx, BOOL is_first_cc)
{
    S_ADDRX cc_base = is_first_cc ? _cc1_base : _cc2_base;
    S_ADDRX ret_addrx = find_cc_saddrx_from_orig(orig_p_addrx, is_first_cc);
    return ret_addrx!=0 ? (ret_addrx - cc_base + _cc_load_base) : 0;
}

P_ADDRX CodeVariantManager::find_cc_paddrx_from_rbbl(RandomBBL *rbbl, BOOL is_first_cc)
{
    RBBL_CC_MAPS &rbbl_maps = is_first_cc ? _rbbl_maps1 : _rbbl_maps2;    
    CC_LAYOUT &cc_layout = is_first_cc ? _cc_layout1 : _cc_layout2;
    F_SIZE rbbl_offset = rbbl->get_rbbl_offset();
    S_ADDRX cc_base = is_first_cc ? _cc1_base : _cc2_base;
    
    RBBL_CC_MAPS::iterator iter = rbbl_maps.find(rbbl_offset);
    if(iter!=rbbl_maps.end()){
        S_ADDRX s_addrx = iter->second;
        CC_LAYOUT_ITER it = cc_layout.lower_bound(Range<S_ADDRX>(s_addrx));
        return (it!=cc_layout.end())&&(it->second==(S_ADDRX)rbbl)? (_cc_load_base + s_addrx - cc_base) : 0; 
    }else
        return 0;
}

P_ADDRX CodeVariantManager::find_cc_paddrx_from_all_rbbls(RandomBBL *rbbl, BOOL is_first_cc)
{
    ASSERT(is_first_cc ? _is_cv1_ready : _is_cv2_ready);

    for(CVM_MAPS::iterator iter = _all_cvm_maps.begin(); iter!=_all_cvm_maps.end(); iter++){
        P_ADDRX ret_addrx = iter->second->find_cc_paddrx_from_rbbl(rbbl, is_first_cc);
        if(ret_addrx!=0)
            return ret_addrx;
    }
    return 0;
}

P_ADDRX CodeVariantManager::get_new_pc_from_old(P_ADDRX old_pc, BOOL first_cc_is_new)
{
    RandomBBL *rbbl = find_rbbl_from_paddrx(old_pc, first_cc_is_new ? false : true);
    if(rbbl){
        //get old and new code variant information
        RBBL_CC_MAPS &old_rbbl_maps = first_cc_is_new ? _rbbl_maps2 : _rbbl_maps1;
        RBBL_CC_MAPS &new_rbbl_maps = first_cc_is_new ? _rbbl_maps1 : _rbbl_maps2;
        S_ADDRX old_cc_base = first_cc_is_new ? _cc2_base : _cc1_base;
        S_ADDRX new_cc_base = first_cc_is_new ? _cc1_base : _cc2_base;
        //get old rbbl offset
        RBBL_CC_MAPS::iterator it = old_rbbl_maps.find(rbbl->get_rbbl_offset());
        ASSERT(it!=old_rbbl_maps.end());
        S_ADDRX old_pc_saddrx = old_pc - _cc_load_base + old_cc_base;
        S_SIZE rbbl_internal_offset = old_pc_saddrx - it->second;
        //get new rbbl saddrx
        it = new_rbbl_maps.find(rbbl->get_rbbl_offset());
        ASSERT(it!=new_rbbl_maps.end());
        S_ADDRX new_pc_saddrx = it->second + rbbl_internal_offset;
        return new_pc_saddrx - new_cc_base + _cc_load_base;
    }else
        return 0;
}

P_ADDRX CodeVariantManager::get_new_pc_from_old_all(P_ADDRX old_pc, BOOL first_cc_is_new)
{
    ASSERT(_is_cv1_ready&&_is_cv2_ready);

    for(CVM_MAPS::iterator iter = _all_cvm_maps.begin(); iter!=_all_cvm_maps.end(); iter++){
        P_ADDRX new_pc = iter->second->get_new_pc_from_old(old_pc, first_cc_is_new);
        if(new_pc!=0)
            return new_pc;                
    }
    
    return 0;
}

void CodeVariantManager::modify_new_ra_in_ss(BOOL first_cc_is_new)
{
    //TODO: we only handle ordinary shadow stack, not shadow stack++
    ASSERT(_is_cv1_ready && _is_cv2_ready);
    S_ADDRX return_addr_ptr = _ss_base - sizeof(P_ADDRX);
    
    while(return_addr_ptr>=(_ss_base-_ss_load_size)){
        P_ADDRX old_return_addr = *(P_ADDRX *)return_addr_ptr;
        P_ADDRX new_return_addr = get_new_pc_from_old_all(old_return_addr, first_cc_is_new);
        //modify old return address to the new return address
        if(new_return_addr!=0)
            *(P_ADDRX *)return_addr_ptr = new_return_addr;
        
        return_addr_ptr -= sizeof(P_ADDRX);
    }
}

