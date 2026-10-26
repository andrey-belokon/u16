import os
from playwright.sync_api import sync_playwright
from pathlib import Path
import io, zipfile
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from threading import Thread
import functools
html=Path(__file__).resolve().parent.parent/'cpu16_debugger.html'
server=ThreadingHTTPServer(('127.0.0.1',0),functools.partial(SimpleHTTPRequestHandler,directory=str(html.parent)))
Thread(target=server.serve_forever,daemon=True).start()
with sync_playwright() as p:
    # the path to Chromium is from CHROMIUM, otherwise the Playwright default browser
    exe=os.environ.get('CHROMIUM')
    browser=p.chromium.launch(executable_path=exe, headless=True, args=['--no-sandbox']) if exe else p.chromium.launch(headless=True)
    context=browser.new_context(accept_downloads=True)
    page=context.new_page()
    errors=[]
    page.on('pageerror',lambda err: errors.append(str(err)))
    page.set_content(html.read_text(encoding='utf8'),wait_until='domcontentloaded')
    page.locator('#tab-disks').click()
    page.locator('button[data-act="new"][data-n="0"]').click()
    page.locator('#dk-sectors').fill('9000')
    page.locator('button[data-act="create"][data-n="0"]').click()
    assert 'Files…' in page.locator('#dk-rows').inner_text()
    with page.expect_popup() as info:
        page.locator('button[data-act="files"][data-n="0"]').click()
    pop=info.value
    pop.wait_for_load_state()
    assert pop.locator('#format').is_visible(),pop.locator('body').inner_text()
    pop.once('dialog',lambda d:d.accept())
    pop.locator('#format').click()
    assert pop.locator('#format').is_hidden(), pop.locator('#message').inner_text()
    # Two files + directory operations and multi-select
    pop.locator('#files').set_input_files([
      {'name':'hello.bin','mimeType':'application/octet-stream','buffer':b'hello'*1000},
      {'name':'second.txt','mimeType':'text/plain','buffer':b'FS16 works!'}])
    assert pop.locator('#local-list .item').count()==2
    pop.locator('#local-list .item').first.drag_to(pop.locator('#disk-list'))
    pop.wait_for_function("document.querySelector('#message').textContent.includes('Copied ')")
    assert pop.locator('#disk-list .item').count()==2,pop.locator('#message').inner_text()
    assert 'Copied 2 file(s)' in pop.locator('#message').inner_text(),pop.locator('#message').inner_text()
    pop.locator('#disk-list .item').nth(0).click()
    pop.locator('#disk-list .item').nth(1).click(modifiers=['Control'])
    with page.expect_download() as download_info:
        pop.locator('#download').click()
    dl=download_info.value
    assert dl.suggested_filename=='fs16-files.zip',dl.suggested_filename
    z=zipfile.ZipFile(io.BytesIO(Path(dl.path()).read_bytes()))
    assert z.read('hello.bin')==b'hello'*1000
    assert z.read('second.txt')==b'FS16 works!'
    with page.expect_download() as drag_download:
        pop.locator('#disk-list .item').first.drag_to(pop.locator('#local-list'))
    assert drag_download.value.suggested_filename=='fs16-files.zip'
    pop.once('dialog',lambda d:d.accept('MORE'))
    pop.locator('#mkdir').click()
    pop.locator('#disk-list .item').filter(has_text='MORE').dblclick()
    assert pop.locator('#path').inner_text()=='/MORE'
    pop.locator('#files').set_input_files({'name':'nested.bin','mimeType':'application/octet-stream','buffer':b'N'*45000})
    pop.locator('#upload').click()
    pop.wait_for_function("document.querySelector('#message').textContent.includes('Copied ')")
    assert 'nested.bin' in pop.locator('#disk-list').inner_text()
    pop.locator('#up').click()
    assert pop.locator('#disk-list .item').count()==3
    pop.locator('#files').set_input_files({'name':'into-folder.txt','mimeType':'text/plain','buffer':b'Dropped into MORE'})
    pop.locator('#local-list .item').last.drag_to(pop.locator('#disk-list .item').filter(has_text='MORE'))
    pop.wait_for_function("document.querySelector('#message').textContent.includes('Copied ')")
    pop.locator('#disk-list .item').filter(has_text='MORE').dblclick()
    assert 'into-folder.txt' in pop.locator('#disk-list').inner_text()
    pop.locator('#up').click()
    assert 'modified' in page.locator('#dk-rows').inner_text()
    image_ok = page.evaluate("(() => {const x=new Fs16(engine.getDiskImage(0),false,MACHINE.charset); return x.list(['MORE']).some(e=>e.name==='nested.bin' && e.size===45000)})()")
    assert image_ok
    page.locator('input[data-act="ro"][data-n="0"]').check()
    pop.locator('#refresh').click()
    assert pop.locator('#upload').is_disabled()
    assert 'Read-only' in pop.locator('#warning').inner_text()
    page.locator('input[data-act="ro"][data-n="0"]').uncheck()
    page.evaluate("(() => {const x=new Fs16(engine.getDiskImage(0),false,MACHINE.charset);x.w16(14,1);x.checksum()})()")
    pop.locator('#refresh').click()
    assert pop.locator('#upload').is_disabled() and 'DIRTY' in pop.locator('#warning').inner_text()
    page.evaluate("(() => {const x=new Fs16(engine.getDiskImage(0),false,MACHINE.charset);x.w16(14,0);x.checksum()})()")
    print('Browser integration: popup, format, bulk copy, bulk ZIP download, folder navigation, nested upload, image mutation — passed')
    assert not errors,errors
    browser.close()
