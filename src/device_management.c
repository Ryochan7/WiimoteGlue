#include <stdio.h>
#include <errno.h>
#include <linux/input.h>
#include <string.h>
#include <unistd.h>

#include "wiimoteglue.h"

/*This logic was a bit too ugly
 *to place in the other files,
 *so here it is.
 */

int add_wii_device(struct wiimoteglue_state *state, char* syspath, const char* uniq) {
  int i;
  struct xwii_iface *wiidev;
  if (syspath == NULL)
    return -1;
  xwii_iface_new(&wiidev,syspath);
  xwii_iface_watch(wiidev,true);
  xwii_iface_open(wiidev,  XWII_IFACE_WRITABLE | (XWII_IFACE_ALL ^ XWII_IFACE_ACCEL ^ XWII_IFACE_IR));
  if (!(xwii_iface_available(wiidev) & (XWII_IFACE_CORE | XWII_IFACE_PRO_CONTROLLER | XWII_IFACE_BALANCE_BOARD))) {
    printf("Tried to add a non-wiimote device...\n");
    printf("Or we didn't have permission on the event devices?\n");
    printf("You might need to add a udev rule to give permission.\n");
    xwii_iface_unref(wiidev);
    return -1;
  }


  struct wii_device_list *list_node = calloc(1,sizeof(struct wii_device_list));
  struct wii_device *dev= calloc(1,sizeof(struct wii_device));



  list_node->dev = dev;
  dev->main_list = list_node;


  dev->xwii = wiidev;
  dev->ifaces = xwii_iface_opened(wiidev);

  dev->slot_list = calloc(1,sizeof(struct wii_device_list));
  dev->slot_list->dev = dev;


  if (state->ignore_pro && (dev->ifaces & XWII_IFACE_PRO_CONTROLLER)) {
    xwii_iface_unref(wiidev);
    free(list_node);
    free(dev);
    return 0;
  }



  dev->type = REMOTE;
  if (dev->ifaces & XWII_IFACE_BALANCE_BOARD) {
    dev->type = BALANCE;
  }
  if (dev->ifaces & XWII_IFACE_PRO_CONTROLLER) {
    dev->type = PRO;
  }

  dev->fd = xwii_iface_get_fd(wiidev);
  wiimoteglue_epoll_watch_wiimote(state->epfd, dev);

  store_led_state(state,dev);

  if (state->num_slots > 0) {
    struct virtual_controller *slot = find_open_slot(state,dev->type);
    add_device_to_slot(state,dev,slot);
  } else {
    add_device_to_slot(state,dev,&state->slots[0]);
    /*If the user has no virtual gamepads,
     *they likely want devices to automatically
     *go to the keyboardmouse
     */
  }



  if (dev->slot == NULL) {
    if (dev->type == BALANCE) {
      printf("Balance board detected, but there's no open slots\n");
    } else {
      printf("Controller detected, but there's no open slots\n");
    }
    printf("(WiimoteGlue still has it open and listening...)\n");
  } else {
    if (dev->type == BALANCE) {
      printf("Balance Board added to slot %s\n",dev->slot->slot_name);
    } else {
      printf("Controller added to slot %s\n",dev->slot->slot_name);
    }

  }


  compute_device_map(state,dev);

  if (dev->map->accel_active) {
    xwii_iface_open(wiidev,XWII_IFACE_ACCEL);
  } else {
    xwii_iface_close(wiidev,XWII_IFACE_ACCEL);
  }

  if (dev->map->IR_count) {
    xwii_iface_open(wiidev,XWII_IFACE_IR);
  } else {
    xwii_iface_close(wiidev,XWII_IFACE_IR);
  }

  dev->bluetooth_addr = malloc(18*sizeof(char));
  strncpy(dev->bluetooth_addr, uniq, 17);
  dev->bluetooth_addr[17] = '\0';

  dev->id = calloc(WG_MAX_NAME_SIZE,sizeof(char));
  snprintf(dev->id,WG_MAX_NAME_SIZE,"dev%d",++(state->dev_count));

  printf("\tid: %s\n\taddress %s\n",dev->id, dev->bluetooth_addr);




  list_node->next = &state->dev_list;
  list_node->prev = state->dev_list.prev;

  if (list_node->next != NULL)
    list_node->next->prev = list_node;

  if (list_node->prev != NULL)
    list_node->prev->next = list_node;





  return 0;

}

int close_wii_device(struct wiimoteglue_state* state, struct wii_device *dev) {
    printf("Controller %s (%s) has been removed.\n",dev->id,dev->bluetooth_addr);

    set_led_state(state,dev,dev->original_leds);
    close(dev->fd);

    xwii_iface_unref(dev->xwii);
    if (dev->slot != NULL) {
      printf("(It was assigned slot %s)\n",dev->slot->slot_name);
      remove_device_from_slot(dev);
    }


    struct wii_device_list *list = dev->main_list;
    if (list->prev) {
      list->prev->next = list->next;
    }
    if (list->next) {
      list->next->prev = list->prev;
    }

    if (dev->id != NULL) {
      free(dev->id);
    }
    if (dev->bluetooth_addr != NULL) {
      free(dev->bluetooth_addr);
    }

    free(dev->slot_list);
    free(dev);
    free(list);

    return 0;
}


int store_led_state(struct wiimoteglue_state* state, struct wii_device *dev) {
  if (dev == NULL)
    return -1;
  if (state->set_leds == 0 || dev->type == BALANCE)
    return 0; /*do nothing*/
  /*This code will have to change
   *if there is ever a wiimote with
   *more than 4 LEDs.
   */
  int i;
  int ret;
  int okay = 0;
  for (i = 1; i <= 4; i++) {
    ret = xwii_iface_get_led(dev->xwii,XWII_LED(i),&dev->original_leds[i-1]);
    if (ret < 0) {
      dev->original_leds[i-1] = -1;
      okay = -1;
    }
  }
  return okay;
}

int set_led_state(struct wiimoteglue_state* state, struct wii_device *dev, bool leds[]) {
  /*This code will have to change
   *if there is ever a wiimote with
   *more than 4 LEDs.
   */

  if (dev == NULL || leds == NULL)
    return -1;
  if (state->set_leds == 0 || dev->type == BALANCE)
    return 0; /*do nothing*/
  int i;
  int ret;
  int okay = 0;

  for (i = 1; i <= 4; i++) {
    if (leds[i-1] < 0)
      return -1;
    /*if anything seems fishy, bail out.
     *We really don't want to end up with
     *a device connected but with all 4
     *leds off.
     */
  }

  for (i = 1; i <= 4; i++) {
    ret = xwii_iface_set_led(dev->xwii,XWII_LED(i),leds[i-1]);
    if (ret < 0) {
      okay = -1;
    }
  }
  return okay;
}