"""CreateNew Premiere review videos; one manifest TGA per video frame, no resampling.

Source frames remain immutable. MOV is clean ProRes 4444; MP4 adds a header with
zero-based video and renderer frame numbers. CFR playback is deliberately slower
than the original high-FPS QPC cadence. CSV retains exact source timing/identity.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time

def require(ok, message):
    if not ok: raise ValueError(message)

def strict_pairs(pairs):
    out={}
    for k,v in pairs:
        require(k not in out,'duplicate manifest key: '+k);out[k]=v
    return out

def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest().upper()

def tc(index, fps):
    seconds,frame=divmod(index,fps)
    minute,second=divmod(seconds,60);hour,minute=divmod(minute,60)
    return f'{hour:02}:{minute:02}:{second:02}:{frame:02}'

def validate(source):
    manifest=source/'manifest.json';pin=sha(manifest)
    data=json.loads(manifest.read_text(encoding='utf-8'),object_pairs_hook=strict_pairs)
    rows=data['frames'];width,height=data['width'],data['height'];frequency=int(data['qpcFrequency'])
    require(rows and len(rows)==data['retained']==data['saved'],'manifest frame count')
    require(data['state']==6 and data['failed']==0 and frequency>0,'unfinished history')
    paths=[];receipts=[];last=None;seen=set()
    expected_header=bytearray(18);expected_header[2]=2
    struct.pack_into('<HH',expected_header,12,width,height);expected_header[16]=32;expected_header[17]=0x28
    for row in rows:
        path=(source/Path(row['file']).name).resolve()
        require(path.parent==source and path.name not in seen,'duplicate/escaping source path');seen.add(path.name)
        require(row['saved'] is True,'unsaved source image')
        require(path.stat().st_size==18+width*height*4,'unexpected TGA size')
        with path.open('rb') as f:require(f.read(18)==expected_header,'not a supported native BGRA TGA')
        current=(int(row['ordinal']),int(row['present']),int(row['frame']),int(row['qpc']))
        if last:
            require(current[:3]==tuple(x+1 for x in last[:3]),'nonconsecutive image/render/Present frames')
            require(current[3]>last[3],'QPC regression')
        last=current;paths.append(path)
        receipts.append(dict(file=path.name,bytes=path.stat().st_size,sha256=sha(path)))
    require(seen=={p.name for p in source.glob('*.tga')},'unlisted TGA files')
    require(sha(manifest)==pin,'manifest changed during validation')
    return data,paths,receipts,pin

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--ffmpeg',type=Path,default=Path('E:/Dev/ffmpeg-7.1.1-essentials_build/bin/ffmpeg.exe'))
    p.add_argument('--fps',type=int,default=60)
    a=p.parse_args();require(a.fps in (24,25,30,50,60),'unsupported review frame rate')
    source=a.source.resolve();out=a.output.resolve();require(not out.exists(),'output already exists')
    ffmpeg=a.ffmpeg.resolve();ffprobe=ffmpeg.with_name('ffprobe.exe')
    require(ffmpeg.is_file() and ffprobe.is_file(),'FFmpeg tools missing')
    data,paths,receipts,manifest_pin=validate(source)
    out.mkdir(parents=True,exist_ok=False)
    n=len(paths);w,h=data['width'],data['height'];first=int(data['frames'][0]['frame'])
    clean=out/'history_clean_60fps.mov';numbered=out/'history_numbered_60fps.mp4'
    if a.fps!=60:
        clean=out/f'history_clean_{a.fps}fps.mov';numbered=out/f'history_numbered_{a.fps}fps.mp4'
    font=Path('C:/Windows/Fonts/consola.ttf');require(font.exists(),'counter font missing')
    font_filter=font.as_posix().replace(':',r'\:')
    label=f'VIDEO %{{eif\\:n\\:d}}  |  RENDER %{{eif\\:n+{first}\\:d}}'
    filters=(f"[0:v]split=2[c][n];[c]scale=in_range=full:out_range=tv:out_color_matrix=bt709,format=yuv444p10le[clean];"
             f"[n]pad=iw:ih+64:0:64:color=black,drawtext=fontfile='{font_filter}':text='{label}':"
             'x=20:y=14:fontsize=32:fontcolor=white,'
             'scale=in_range=full:out_range=tv:out_color_matrix=bt709,format=yuv420p[numbered]')
    colors=['-color_primaries','bt709','-color_trc','iec61966-2-1','-colorspace','bt709','-color_range','tv']
    cmd=[str(ffmpeg),'-hide_banner','-nostdin','-n','-loglevel','error',
         '-f','rawvideo','-pixel_format','bgra','-video_size',f'{w}x{h}','-framerate',str(a.fps),'-i','pipe:0',
         '-filter_complex_threads','2','-filter_complex',filters,
         '-map','[clean]','-an','-c:v','prores_ks','-profile:v','4','-vendor','apl0','-alpha_bits','0','-threads','2',
         '-frames:v',str(n),'-fps_mode','passthrough','-video_track_timescale','60000','-timecode','00:00:00:00',
         *colors,str(clean),
         '-map','[numbered]','-an','-c:v','libx264','-preset','fast','-crf','16','-g','1','-bf','0','-threads','2',
         '-frames:v',str(n),'-fps_mode','passthrough','-video_track_timescale','60000','-movflags','+faststart',
         *colors,str(numbered)]
    with (out/'encode-command.json').open('x',encoding='utf-8') as f:json.dump(cmd,f,indent=2)
    started=time.monotonic();pipe_error=None
    with (out/'encode.log').open('xb') as log:
        process=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=log)
        try:
            for index,path in enumerate(paths):
                with path.open('rb') as f:
                    f.seek(18)
                    while chunk:=f.read(1024*1024):process.stdin.write(chunk)
                if (index+1)%25==0:print(f'Encoded input {index+1}/{n}',flush=True)
        except (BrokenPipeError,OSError) as exc:pipe_error=str(exc)
        finally:
            try:process.stdin.close()
            except BrokenPipeError:pass
        code=process.wait()
    require(code==0 and pipe_error is None,'FFmpeg failed; inspect encode.log; partial output preserved')
    outputs=[]
    for path,expected_height in ((clean,h),(numbered,h+64)):
        probe=json.loads(subprocess.check_output([str(ffprobe),'-v','error','-select_streams','v:0','-count_frames',
            '-show_entries','stream=codec_name,profile,width,height,r_frame_rate,avg_frame_rate,nb_read_frames,pix_fmt,duration',
            '-of','json',str(path)],text=True))
        s=probe['streams'][0]
        require(int(s['nb_read_frames'])==n and s['width']==w and s['height']==expected_height,'encoded frame count/extent')
        require(s['avg_frame_rate']==f'{a.fps}/1','encoded rate mismatch')
        decoded=subprocess.run([str(ffmpeg),'-v','error','-xerror','-i',str(path),'-map','0:v:0','-an','-f','null','-'],capture_output=True)
        require(decoded.returncode==0,'video decode error')
        outputs.append(dict(file=path.name,bytes=path.stat().st_size,sha256=sha(path),probe=s))
    for path,pin in zip(paths,receipts):
        require(path.stat().st_size==pin['bytes'] and sha(path)==pin['sha256'],'source image changed')
    require(sha(source/'manifest.json')==manifest_pin,'source manifest changed')
    with (out/'frame_mapping.csv').open('x',encoding='utf-8-sig',newline='') as f:
        writer=csv.writer(f);writer.writerow(['video_frame_0based','video_frame_1based','premiere_timecode',
            'render_frame','present','history_ordinal','source_elapsed_ms','source_qpc','tga_file','tga_sha256'])
        firstq=int(data['frames'][0]['qpc']);freq=int(data['qpcFrequency'])
        for i,(row,pin) in enumerate(zip(data['frames'],receipts)):
            writer.writerow([i,i+1,tc(i,a.fps),row['frame'],row['present'],row['ordinal'],
                f'{(int(row["qpc"])-firstq)*1000/freq:.6f}',row['qpc'],pin['file'],pin['sha256']])
    original_span=(int(data['frames'][-1]['qpc'])-int(data['frames'][0]['qpc']))/int(data['qpcFrequency'])
    result=dict(source=str(source),sourceManifestSha256=manifest_pin,sourceImagesUnchanged=True,
                frames=n,reviewFps=a.fps,reviewDurationSeconds=n/a.fps,sourceFirstToLastSeconds=original_span,
                firstRenderFrame=first,lastRenderFrame=int(data['frames'][-1]['frame']),outputs=outputs,
                sourceImages=receipts,processingSeconds=time.monotonic()-started)
    with (out/'video_manifest.json').open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    readme=(f'# PR逐帧检查视频\n\n原始TGA和日志未修改。共{n}张，按manifest顺序，一张对应一帧。\n\n'
            f'- {clean.name}：无标记ProRes4444 MOV，原分辨率{w}×{h}。\n'
            f'- {numbered.name}：H.264全I帧预览，顶部额外64像素显示VIDEO/RENDER编号，不遮挡原图。\n'
            f'- frame_mapping.csv：视频帧、PR时间码、render/Present/ordinal及原始QPC时间。\n\n'
            f'视频为{a.fps}fps，时长{n/a.fps:.6f}秒；原始首末帧间隔{original_span:.7f}秒。'
            '这是故意放慢的逐帧封装，不是实时速度，不丢帧/补帧/插值。\n'
            f'PR请使用匹配素材的{a.fps}fps序列；时间码从00:00:00:00开始。'
            f'本段渲染帧 = {first} + 视频帧序号（从0开始）。\n\n'
            '编码经过RGB→YUV转换；视频用于定位，原始TGA才是逐像素证据。'
            '发现问题后提供RENDER编号或CSV对应行即可，不必重新抓图。\n')
    with (out/'README.md').open('x',encoding='utf-8') as f:f.write(readme)
    print(json.dumps({k:v for k,v in result.items() if k!='sourceImages'},indent=2))

if __name__=='__main__':main()
