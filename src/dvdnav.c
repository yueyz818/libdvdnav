/* 
 * Copyright (C) 2000 Rich Wareham <richwareham@users.sourceforge.net>
 * 
 * This file is part of libdvdnav, a DVD navigation library.
 * 
 * libdvdnav is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * libdvdnav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 * $Id$
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <pthread.h>
#include <dvdnav.h>
#include "dvdnav_internal.h"
#include "read_cache.h"

#include <dvdread/nav_read.h>

#include <stdlib.h>
#include <stdio.h>

dvdnav_status_t dvdnav_open(dvdnav_t** dest, char *path) {
  dvdnav_t *self;
  
  /* Create a new structure */
  (*dest) = NULL;
  self = (dvdnav_t*)malloc(sizeof(dvdnav_t));
  if(!self)
   return S_ERR;
  memset(self, 0, (sizeof(dvdnav_t) ) ); /* Make sure self structure is clean */

  pthread_mutex_init(&self->vm_lock, NULL);
  /* Initialise the error string */
  printerr("");

  /* Initialise the VM */
  self->vm = vm_new_vm();
  if(!self->vm) {
    printerr("Error initialising the DVD VM");
    return S_ERR;
  }
  if(vm_reset(self->vm, path) == -1) {
    printerr("Error starting the VM / opening the DVD device");
    return S_ERR;
  }

  /* Set the path. FIXME: Is a deep copy 'right' */
  strncpy(self->path, path, MAX_PATH_LEN);

  /* Set initial values of flags */
  self->expecting_nav_packet = 1;
  self->started = 0;
  
  self->open_vtsN = -1;
  self->open_domain = -1;
  self->file = NULL;
  self->cell = NULL;
  self->at_soc = 1;
  self->jumping = 0;
  self->seeking = 0;
  self->still_frame = -1;
  self->cache_buffer = NULL;
  self->cache_start_sector = -1;
  self->cache_block_count = 0;
  self->cache_valid = 0;
  self->use_read_ahead = 1;
  self->stop = 0;
  self->highlight_changed = 0;
  self->spu_clut_changed = 0;

  self->vobu_start = self->vobu_length = 0;
 
  /* Pre-open and close a file so that the CSS-keys are cached. */
  self->file = DVDOpenFile(vm_get_dvd_reader(self->vm), 0, DVD_READ_MENU_VOBS);
  if (self->file) DVDCloseFile(self->file);
  self->file = NULL;
    
  if(!self->started) {
    /* Start the VM */
    vm_start(self->vm);
    self->started = 1;
  }

  (*dest) = self;
  return S_OK;
}

dvdnav_status_t dvdnav_close(dvdnav_t *self) {
  if(!self) {
    printerr("Passed a NULL pointer");
    return S_ERR;
  }
  printf("dvdnav:close:called\n");
  if (self->file) {
    DVDCloseFile(self->file);
    printf("dvdnav:close:file closing\n");
    self->file = NULL;
  }

  /* Free the VM */
  if(self->vm) {
    vm_free_vm(self->vm);
  }
  if (self->file) {
    DVDCloseFile(self->file);
    printf("dvdnav:close2:file closing\n");
    self->file = NULL;
  }
  pthread_mutex_destroy(&self->vm_lock);
  /* Finally free the entire structure */
  free(self);
  
  return S_OK;
}

dvdnav_status_t dvdnav_path(dvdnav_t *self, char** path) {
  if(!self || !path || !(*path)) {
    return S_ERR;
  }

  /* FIXME: Is shallow copy 'right'? */
  (*path) = self->path;

  return S_OK;
}

char* dvdnav_err_to_string(dvdnav_t *self) {
  if(!self) {
    /* Shold this be "passed a NULL pointer?" */
    return NULL;
  }
  
  return self->err_str;
}

/**
 * Returns 1 if block contains NAV packet, 0 otherwise.
 * Precesses said NAV packet if present.
 *
 * Most of the code in here is copied from xine's MPEG demuxer
 * so any bugs which are found in that should be corrected here also.
 */
int dvdnav_check_packet(dvdnav_t *self, uint8_t *p) {
  int            bMpeg1=0;
  uint32_t       nHeaderLen;
  uint32_t       nPacketLen;
  uint32_t       nStreamID;
/* uint8_t       *p_start=p; */


  if (p==NULL) {
    printf("Passed a NULL pointer.\n");
    return 0;
  }

  /* dprint("Checking packet...\n"); */

  if (p[3] == 0xBA) { /* program stream pack header */

    int nStuffingBytes;

    /* xprintf (VERBOSE|DEMUX, "program stream pack header\n"); */

    bMpeg1 = (p[4] & 0x40) == 0;

    if (bMpeg1) {
      p   += 12;
    } else { /* mpeg2 */
      nStuffingBytes = p[0xD] & 0x07;
      p += 14 + nStuffingBytes;
    }
  }


  if (p[3] == 0xbb) { /* program stream system header */
    int nHeaderLen;

    nHeaderLen = (p[4] << 8) | p[5];
    p += 6 + nHeaderLen;
  }

  /* we should now have a PES packet here */

  if (p[0] || p[1] || (p[2] != 1)) {
    printf("demux error! %02x %02x %02x (should be 0x000001) \n",p[0],p[1],p[2]);
    return 0;
  }

  nPacketLen = p[4] << 8 | p[5];
  nStreamID  = p[3];

  nHeaderLen = 6;
  p += nHeaderLen;

  if (nStreamID == 0xbf) { /* Private stream 2 */
/*
 *   int i;
 *    printf("dvdnav:nav packet=%u\n",p-p_start-6);
 *   for(i=0;i<80;i++) {
 *     printf("%02x ",p[i-6]);
 *   }
 *   printf("\n");
 */
    if(p[0] == 0x00) {
#ifdef HAVE_DVDREAD9
      navRead_PCI(&(self->pci), p+1);
#else
      navRead_PCI(&(self->pci), p+1, nPacketLen - 1);
#endif
    }

    p += nPacketLen;

    /* We should now have a DSI packet. */
    if(p[6] == 0x01) {
      int num=0, current=0;

      nPacketLen = p[4] << 8 | p[5];
      p += 6;
      /* dprint("NAV DSI packet\n");  */
#ifdef HAVE_DVDREAD9
      navRead_DSI(&(self->dsi), p+1);
#else
      navRead_DSI(&(self->dsi), p+1, sizeof(dsi_t));
#endif

      self->vobu_start = self->dsi.dsi_gi.nv_pck_lbn;
      self->vobu_length = self->dsi.dsi_gi.vobu_ea;
      
      /**
       * If we're not at the end of this cell, we can determine the next
       * VOBU to display using the VOBU_SRI information section of the
       * DSI.  Using this value correctly follows the current angle,
       * avoiding the doubled scenes in The Matrix, and makes our life
       * really happy.
       *
       * Otherwise, we set our next address past the end of this cell to
       * force the code above to go to the next cell in the program.
       */
      if( self->dsi.vobu_sri.next_vobu != SRI_END_OF_CELL ) {
	self->next_vobu = self->dsi.dsi_gi.nv_pck_lbn 
      	 + ( self->dsi.vobu_sri.next_vobu & 0x7fffffff );
      } else {
        self->next_vobu = self->vobu_start + self->vobu_length;
      }
      
      dvdnav_get_angle_info(self, &current, &num);
      if(num == 1) {
	/* This is to switch back to angle one when we
	 * finish */
	dvdnav_angle_change(self, 1);
      }
      
      if(num != 0) {
	uint32_t next = self->pci.nsml_agli.nsml_agl_dsta[current-1];

       	if(next != 0) {
	  int dir = 0;
	  if(next & 0x80000000) {
	    dir = -1;
	    next = next & 0x3fffffff;
	  } else {
	    dir = 1;
	  }

	  if(next != 0) {
	    self->next_vobu = self->vobu_start + dir * next;
	  }
	} else if( self->dsi.sml_agli.data[current-1].address != 0 ) {
	  next = self->dsi.sml_agli.data[current-1].address;
	  self->vobu_length = self->dsi.sml_pbi.ilvu_ea;

	  if((next & 0x80000000) && (next != 0x7fffffff)) {
	    self->next_vobu = self->dsi.dsi_gi.nv_pck_lbn - (next & 0x7fffffff);
	  } else {
	    self->next_vobu = self->dsi.dsi_gi.nv_pck_lbn + next;
	  }
	}
      }
    }
    return 1;
  }

  return 0;
}

dvdnav_status_t dvdnav_get_next_block(dvdnav_t *self, unsigned char *buf,
 				      int *event, int *len) {
  dvd_state_t *state;
  int result;
  if(!self || !event || !len || !buf) {
    printerr("Passed a NULL pointer");
    return S_ERR;
  }
  pthread_mutex_lock(&self->vm_lock); 
  
  if(!self->started) {
    /* Start the VM */
    vm_start(self->vm);
    self->started = 1;
  }

  state = &(self->vm->state);
  (*event) = DVDNAV_NOP;
  (*len) = 0;
 
  /* Check the STOP flag */
  if(self->stop) {
    (*event) = DVDNAV_STOP;
    pthread_mutex_unlock(&self->vm_lock); 
    return S_OK;
  }

  if(self->spu_clut_changed) {
    (*event) = DVDNAV_SPU_CLUT_CHANGE;
    printf("libdvdnav:SPU_CLUT_CHANGE\n");
    (*len) = sizeof(dvdnav_still_event_t);
    memcpy(buf, &(state->pgc->palette), 16 * sizeof(uint32_t));
    self->spu_clut_changed = 0;
    printf("libdvdnav:SPU_CLUT_CHANGE returning S_OK\n");
    pthread_mutex_unlock(&self->vm_lock); 
    return S_OK;
  }
  
  if(self->spu_stream_changed) {
    dvdnav_stream_change_event_t stream_change;
    (*event) = DVDNAV_SPU_STREAM_CHANGE;
    printf("libdvdnav:SPU_STREAM_CHANGE\n");
    (*len) = sizeof(dvdnav_stream_change_event_t);
    stream_change.physical= vm_get_subp_active_stream( self->vm );
    memcpy(buf, &(stream_change), sizeof( dvdnav_stream_change_event_t));
    self->spu_stream_changed = 0;
    printf("libdvdnav:SPU_STREAM_CHANGE stream_id=%d returning S_OK\n",stream_change.physical);
    pthread_mutex_unlock(&self->vm_lock); 
    return S_OK;
  }
  
  if(self->audio_stream_changed) {
    dvdnav_stream_change_event_t stream_change;
    (*event) = DVDNAV_AUDIO_STREAM_CHANGE;
    printf("libdvdnav:AUDIO_STREAM_CHANGE\n");
    (*len) = sizeof(dvdnav_stream_change_event_t);
    stream_change.physical= vm_get_audio_active_stream( self->vm );
    memcpy(buf, &(stream_change), sizeof( dvdnav_stream_change_event_t));
    self->audio_stream_changed = 0;
    printf("libdvdnav:AUDIO_STREAM_CHANGE stream_id=%d returning S_OK\n",stream_change.physical);
    pthread_mutex_unlock(&self->vm_lock); 
    return S_OK;
  }
     
  /* Check the HIGHLIGHT flag */
  if(self->highlight_changed) {
    dvdnav_highlight_event_t hevent;
   
    /* Fill in highlight struct with appropriate values */
    if(self->hli_state != 0) {
      hevent.display = 1;

      /* Copy current button bounding box. */
      hevent.sx = self->hli_bbox[0];
      hevent.sy = self->hli_bbox[1];
      hevent.ex = self->hli_bbox[2];
      hevent.ey = self->hli_bbox[3];

      hevent.palette = self->hli_clut;
      hevent.pts = self->hli_pts;
      hevent.buttonN = self->hli_buttonN;

    } else {
      hevent.display = 0;
    }
    
    (*event) = DVDNAV_HIGHLIGHT;
    memcpy(buf, &(hevent), sizeof(hevent));
    (*len) = sizeof(hevent);
    
    self->highlight_changed = 0;
    
    pthread_mutex_unlock(&self->vm_lock); 
    return S_OK;
  }

  /* Check to see if we need to change the curently opened VOB */
  if((self->open_vtsN != state->vtsN) || 
     (self->open_domain != state->domain)) {
    dvd_read_domain_t domain;
    int vtsN;
    dvdnav_vts_change_event_t vts_event;
    
    if(self->file) {
      dvdnav_read_cache_clear(self);
      DVDCloseFile(self->file);
      self->file = NULL;
    }

    vts_event.old_vtsN = self->open_vtsN;
    vts_event.old_domain = self->open_domain;
     
    /* Use the current DOMAIN to find whether to open menu or title VOBs */
    switch(state->domain) {
     case FP_DOMAIN:
     case VMGM_DOMAIN:
      domain = DVD_READ_MENU_VOBS;
      vtsN = 0;
      break;
     case VTSM_DOMAIN:
      domain = DVD_READ_MENU_VOBS;
      vtsN = state->vtsN;
      break;
     case VTS_DOMAIN:
      domain = DVD_READ_TITLE_VOBS;
      vtsN = state->vtsN;
      break;
     default:
      printerr("Unknown domain when changing VTS.");
      pthread_mutex_unlock(&self->vm_lock); 
      return S_ERR;
    }
    
    self->open_domain = state->domain;
    self->open_vtsN = state->vtsN;
    dvdnav_read_cache_clear(self);
    self->file = DVDOpenFile(vm_get_dvd_reader(self->vm), vtsN, domain);
    vts_event.new_vtsN = self->open_vtsN;
    vts_event.new_domain = self->open_domain;

    /* If couldn't open the file for some reason, moan */
    if(self->file == NULL) {
      printerrf("Error opening vtsN=%i, domain=%i.", vtsN, domain);
      pthread_mutex_unlock(&self->vm_lock); 
      return S_ERR;
    }

    /* File opened successfully so return a VTS change event */
    (*event) = DVDNAV_VTS_CHANGE;
    memcpy(buf, &(vts_event), sizeof(vts_event));
    (*len) = sizeof(vts_event);

    /* On a VTS change, we want to disable any highlights which
     * may have been shown (FIXME: is this valid?) */
    self->highlight_changed = 1;
    self->spu_clut_changed = 1;
    self->spu_stream_changed = 1;
    self->audio_stream_changed = 1;
    self->hli_state = 0; /* Hide */
    self->expecting_nav_packet = 1;
     
    pthread_mutex_unlock(&self->vm_lock); 
    return S_OK;
  }
 
  /* Check the STILLFRAME flag */
  if(self->still_frame != -1) {
    dvdnav_still_event_t still_event;

    still_event.length = self->still_frame;

    (*event) = DVDNAV_STILL_FRAME;
    (*len) = sizeof(dvdnav_still_event_t);
    memcpy(buf, &(still_event), sizeof(dvdnav_still_event_t));
 
    pthread_mutex_unlock(&self->vm_lock); 
    return S_OK;
  }

  if(self->at_soc) {
    dvdnav_cell_change_event_t cell_event;
    cell_playback_t *cell = &(state->pgc->cell_playback[state->cellN - 1]);
    
    cell_event.old_cell = self->cell;
    self->vobu_start = cell->first_sector;
    self->cell = cell;
    cell_event.new_cell = self->cell;
     
    self->at_soc = 0;

    (*event) = DVDNAV_CELL_CHANGE;
    (*len) = sizeof(dvdnav_cell_change_event_t);
    memcpy(buf, &(cell_event), sizeof(dvdnav_cell_change_event_t));

    pthread_mutex_unlock(&self->vm_lock); 
    return S_OK;
  }

  if(self->expecting_nav_packet) {
    dvdnav_nav_packet_event_t nav_event;

    /* Perform the jump if necessary (this is always a 
     * VOBU boundary). */

    if(self->seeking) {
      /* FIXME:Need to handle seeking outside current cell. */
      vobu_admap_t *admap = NULL;
	
      printf("Seeking to target %u ...\n",
              self->seekto_block);

      /* Search through the VOBU_ADMAP for the nearest VOBU
       * to the target block */
      switch(state->domain) {
        case FP_DOMAIN:
        case VMGM_DOMAIN:
          //ifo = vm_get_vmgi();
          //ifoRead_VOBU_ADMAP(ifo);
          admap = self->vm->vmgi->menu_vobu_admap;
          break;
        case VTSM_DOMAIN:
          //ifo = vm_get_vtsi();
          //ifoRead_VOBU_ADMAP(ifo);
          admap = self->vm->vtsi->menu_vobu_admap;
          break;
        case VTS_DOMAIN:
          //ifo = vm_get_vtsi();
          //ifoRead_TITLE_VOBU_ADMAP(ifo);
          admap = self->vm->vtsi->vts_vobu_admap;
          break;
        default:
          printf("Error: Unknown domain for seeking seek.\n");
      }

      if(admap) {
        uint32_t address = 0;
        uint32_t vobu_start, next_vobu;
        int found = 0;
  
        /* Search through ADMAP for best sector */
        vobu_start = 0x3fffffff;
   
        while((!found) && ((address<<2) < admap->last_byte)) {
          next_vobu = admap->vobu_start_sectors[address];

          /* printf("Found block %u\n", next_vobu); */
    
          if(vobu_start <= self->seekto_block && 
                 next_vobu > self->seekto_block) {
            found = 1;
          } else {
            vobu_start = next_vobu;
          }
    
          address ++;
        }
        if(found) {
          self->vobu_start = vobu_start;
          self->blockN = 0;
          self->seeking = 0;
          //self->at_soc = 1;
          (*event) = DVDNAV_SEEK_DONE;
          (*len) = 0;
          pthread_mutex_unlock(&self->vm_lock); 
          return S_OK;
        } else {
          printf("Could not locate block\n");
          return -1;
        }
      }
    }   
    if(self->jumping) {
      printf("doing jumping\n");
      self->vobu_start = self->jmp_vobu_start;
      self->blockN = self->jmp_blockN;
      self->jumping = 0;
      self->at_soc = 1;
    }
    
    result = DVDReadBlocks(self->file, self->vobu_start + self->blockN, 1, buf);

    if(result <= 0) {
      printerr("Error reading NAV packet.");
      pthread_mutex_unlock(&self->vm_lock); 
      return S_ERR;
    }

    if(dvdnav_check_packet(self, buf) == 0) {
      printerr("Expected NAV packet but none found.");
      pthread_mutex_unlock(&self->vm_lock); 
      return S_ERR;
    }
    
    self->blockN++;
    self->expecting_nav_packet = 0;

    dvdnav_pre_cache_blocks(self, self->vobu_start, self->vobu_length+1);
    
    /* Successfully got a NAV packet */
    nav_event.pci = &(self->pci);
    nav_event.dsi = &(self->dsi);

    (*event) = DVDNAV_NAV_PACKET;
    //memcpy(buf, &(nav_event), sizeof(dvdnav_nav_packet_event_t));
    //(*len) = sizeof(dvdnav_nav_packet_event_t);
    (*len) = 2048;
    pthread_mutex_unlock(&self->vm_lock); 
    return S_OK;
  }
  
  /* If we've got here, it must just be a normal block. */
  if(!self->file) {
    printerr("Attempting to read without opening file");
    pthread_mutex_unlock(&self->vm_lock); 
    return S_ERR;
  }

  result = dvdnav_read_cache_block(self, self->vobu_start + self->blockN, 1, buf);
  if(result <= 0) {
    printerr("Error reading from DVD.");
    pthread_mutex_unlock(&self->vm_lock); 
    return S_ERR;
  }
  self->blockN++;
  (*len) = 2048;
  (*event) = DVDNAV_BLOCK_OK;

  if(self->blockN > self->vobu_length) {
    self->vobu_start = self->next_vobu;
    self->blockN = 0;
    self->expecting_nav_packet = 1;

    if(self->dsi.vobu_sri.next_vobu == SRI_END_OF_CELL) {
      cell_playback_t *cell = &(state->pgc->cell_playback[state->cellN - 1]);

      if(cell->still_time != 0xff) {
        vm_get_next_cell(self->vm);
      }

      if(cell->still_time != 0) {
	self->still_frame = cell->still_time;
      }

      self->at_soc = 1;
    }
  }
  
  pthread_mutex_unlock(&self->vm_lock); 
  return S_OK;
}

uint16_t dvdnav_audio_stream_to_lang(dvdnav_t *self, uint8_t stream) {
  ifo_handle_t *vtsi;
  dvd_state_t  *state;
  
  if(!self)
    return -1;
  
  pthread_mutex_lock(&self->vm_lock); 

  vtsi = self->vm->vtsi;
  state = &(self->vm->state);

  if((vtsi == NULL) || (state == NULL) || (state->domain != VTS_DOMAIN))
    goto __failed;
  
  if(stream >= vtsi->vtsi_mat->nr_of_vts_audio_streams)
    goto __failed;
  
  if(vtsi->vtsi_mat->vts_audio_attr[stream].lang_type != 1)
    goto __failed;
  
  pthread_mutex_unlock(&self->vm_lock); 
  return vtsi->vtsi_mat->vts_audio_attr[stream].lang_code;
  
 __failed:
  pthread_mutex_unlock(&self->vm_lock); 
  return 0xffff;
}

int8_t dvdnav_audio_logical_to_physical(dvdnav_t *self, uint8_t logical) {
  audio_status_t *audio_status;
  dvd_state_t    *state;
  int             i = 0;
  
  if(!self)
    return -1;
  
  pthread_mutex_lock(&self->vm_lock); 
  
  state = &(self->vm->state);
  
  if((!state) || (!state->pgc) || (!state->pgc->audio_control))
    goto __failed;
  
  if(logical > 7) {
    fprintf(stderr, "Invalid logical audio channel: %i\n", logical);
    goto __failed;
  }

  while (i < 8) {
    audio_status = (audio_status_t*) &(state->pgc->audio_control[i]);

    if(!audio_status)
      goto __failed;

    if(audio_status->available)
      break;
    
    i++;
  }
  
  if (i > 7)
    goto __failed;

  if ((logical+i) > 7)
    goto __failed;
  
  audio_status = (audio_status_t*) &(state->pgc->audio_control[logical+i]);

  if(!audio_status)
    goto __failed;
  
  if(audio_status->available) {
    /* Stream is available */
    pthread_mutex_unlock(&self->vm_lock); 
    return audio_status->stream_number; 
  }
  
 __failed:
  pthread_mutex_unlock(&self->vm_lock); 
  return -1;
}

int8_t dvdnav_audio_physical_to_logical(dvdnav_t *self, uint8_t physical) {
  int8_t logical = -1;
  int    i       = 0; 
  
  if((!self) || (physical > 7))
    return -1;
  
  do {
    if(dvdnav_audio_logical_to_physical(self, i) == physical) 
      logical = i;
    
    i++;
  } while((i<8) && (logical == -1));

  return logical;
}

uint16_t dvdnav_spu_stream_to_lang(dvdnav_t *self, uint8_t stream) {
  ifo_handle_t *vtsi;
  dvd_state_t  *state;

  if(!self)
    return -1;
  
  pthread_mutex_lock(&self->vm_lock); 
  
  vtsi = self->vm->vtsi;
  state = &(self->vm->state);

  if((vtsi == NULL) || (state == NULL) || (state->domain != VTS_DOMAIN)) {
    goto __failed;
  }

  if(stream >= vtsi->vtsi_mat->nr_of_vts_subp_streams) {
    goto __failed;
  }

  if(vtsi->vtsi_mat->vts_subp_attr[stream].type != 1) {
    goto __failed;
  }
  
  pthread_mutex_unlock(&self->vm_lock); 
  return vtsi->vtsi_mat->vts_subp_attr[stream].lang_code;
  
 __failed:
  pthread_mutex_unlock(&self->vm_lock); 
  return 0xffff;
}

int8_t dvdnav_spu_logical_to_physical(dvdnav_t *self, uint8_t logical) {
  spu_status_t *spu_status;
  dvd_state_t  *state;
  ifo_handle_t *vtsi;
 
  if(!self)
    return -1;
  
  pthread_mutex_lock(&self->vm_lock); 
  
  vtsi = self->vm->vtsi;
  state = &(self->vm->state);
  
  if((!state) || (!vtsi))
    goto __failed;

  if(logical > 31) {
    fprintf(stderr, "Invalid logical spu channel: %i\n", logical);
    goto __failed;
  }
  
  spu_status = (spu_status_t*) &(state->pgc->subp_control[logical]);
  
  if(!spu_status)
    goto __failed;
  
  if(spu_status->available) {
    int8_t        logical = -1;
    video_attr_t *attr;
    
    attr = &(vtsi->vtsi_mat->vtsm_video_attr);
  
    if(!attr)
      goto __failed;

    /* Stream is available */
    switch(attr->display_aspect_ratio) {
    case 0: /* 4:3 */
      logical = spu_status->stream_number_4_3;
      break;
    case 3: /* 16:9 */
      logical = spu_status->stream_number_letterbox;
      break;
    }
    pthread_mutex_unlock(&self->vm_lock); 
    return logical; 
  }
  
 __failed:
  pthread_mutex_unlock(&self->vm_lock); 
  return -1;
}

int8_t dvdnav_spu_physical_to_logical(dvdnav_t *self, uint8_t physical) {
  int8_t logical = -1;
  int    i       = 0;
  
  if(physical > 31)
    return -1;

  do {
    if(dvdnav_spu_logical_to_physical(self, i) == physical) 
      logical = i;
    
    i++;
  } while((i<32) && (logical == -1));

  return logical;
}

/* Current domain (backend to dvdnav_is_domain_() funcs) */
static int8_t _dvdnav_is_domain(dvdnav_t *self, domain_t domain) {
  dvd_state_t  *state;
  
  if((!self) || (!self->started) || (!self->vm))
    return -1;
  
  pthread_mutex_lock(&self->vm_lock); 
  state = &(self->vm->state);
  pthread_mutex_unlock(&self->vm_lock);

  if(!state)
    return -1;

  return (state->domain == domain) ? 1 : 0;
}

/* First Play domain. (Menu) */
int8_t dvdnav_is_domain_fp(dvdnav_t *self) {
  return _dvdnav_is_domain(self, FP_DOMAIN);
}
/* Video management Menu domain. (Menu) */
int8_t dvdnav_is_domain_vmgm(dvdnav_t *self) {
  return _dvdnav_is_domain(self, VMGM_DOMAIN);
}
/* Video Title Menu domain (Menu) */
int8_t dvdnav_is_domain_vtsm(dvdnav_t *self) {
  return _dvdnav_is_domain(self, VTSM_DOMAIN);
}
/* Video Title domain (playing movie). */
int8_t dvdnav_is_domain_vts(dvdnav_t *self) { 
  return _dvdnav_is_domain(self, VTS_DOMAIN);
}

/* Generally delegate angle information handling to 
 * VM */
dvdnav_status_t dvdnav_angle_change(dvdnav_t *self, int angle) {
  int num, current;
  
  if(!self) {
    return S_ERR;
  }

  if(dvdnav_get_angle_info(self, &current, &num) != S_OK) {
    printerr("Error getting angle info");
    return S_ERR;
  }
  
  /* Set angle SPRM if valid */
  if((angle > 0) && (angle <= num)) {
    self->vm->state.AGL_REG = angle;
  } else {
    printerr("Passed an invalid angle number");
    return S_ERR;
  }

  return S_OK;
}

dvdnav_status_t dvdnav_get_angle_info(dvdnav_t *self, int* current_angle,
				     int *number_of_angles) {
  if(!self || !self->vm) {
    return S_ERR;
  }

  if(!current_angle || !number_of_angles) {
    printerr("Passed a NULL pointer");
    return S_ERR;
  }

  vm_get_angle_info(self->vm, number_of_angles, current_angle);

  return S_OK;
}


/*
 * $Log$
 * Revision 1.1  2002/03/12 19:45:57  richwareham
 * Initial revision
 *
 * Revision 1.28  2002/02/02 23:26:20  richwareham
 * Restored title selection
 *
 * Revision 1.27  2002/02/01 15:48:10  richwareham
 * Re-implemented angle selection and title/chapter display
 *
 * Revision 1.26  2002/01/31 16:53:49  richwareham
 * Big patch from Daniel Caujolle-Bert to (re)implement SPU/Audio language display
 *
 * Revision 1.25  2002/01/24 20:53:50  richwareham
 * Added option to _not_ use DVD read-ahead to options
 *
 * Revision 1.24  2002/01/20 15:54:59  jcdutton
 * Implement seeking.
 * It is still a bit buggy, but works sometimes.
 * I need to find out how to make the jump clean.
 * At the moment, some corruption of the mpeg2 stream occurs, 
 * which causes libmpeg2 to crash.
 *
 * Revision 1.23  2002/01/18 00:23:52  jcdutton
 * Support Ejecting of DVD.
 * It will first un-mount the DVD, then eject it.
 *
 * Revision 1.22  2002/01/17 14:50:32  jcdutton
 * Fix corruption of stream during menu transitions.
 * Menu transitions are now clean.
 *
 * Revision 1.21  2002/01/15 00:37:03  jcdutton
 * Just a few cleanups, and a assert fix. (memset fixed it)
 *
 * Revision 1.20  2002/01/13 22:17:57  jcdutton
 * Change logging.
 *
 *
 */
