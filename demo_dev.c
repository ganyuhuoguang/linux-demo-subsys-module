/*
 * demo subsystem, dev interface
 *
*/

#include <linux/module.h>
#include <linux/sched.h>


#include "demo.h"
#include "demo_dev.h"
#include "demo-core.h"

static dev_t demo_devt;

#define DEMO_DEV_MAX	16			/* ���֧���豸�� */

static int demo_dev_open(struct inode *inode, struct file *file)
{
	struct demo_device *demo = container_of(inode->i_cdev, struct demo_device, char_dev);
	const struct demo_class_ops *ops = demo->ops;
	int err;

	if (test_and_set_bit_lock(DEMO_DEV_BUSY, &demo->flags))
		return -EBUSY;

	file->private_data = demo;

	/* ���������� open ʵ�� */
	err = ops->open ? ops->open(demo->dev.parent) : 0;
	if (err == 0) {
		spin_lock_irq(&demo->irq_lock);
		/* do something while open */
		demo->irq_data = 0;
		spin_unlock_irq(&demo->irq_lock);

		return 0;
	}

	clear_bit_unlock(DEMO_DEV_BUSY, &demo->flags);
	return err;
}

static ssize_t demo_dev_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct demo_device *demo = file->private_data;
	
	DECLARE_WAITQUEUE(wait, current);
	unsigned long data;
	ssize_t ret;

	/* �Զ�ȡ���������б��� */
	if (count != sizeof(unsigned int) && count < sizeof(unsigned long))
		return -EINVAL;

	/* �ȴ����ݾ��� */
	add_wait_queue(&demo->irq_queue, &wait);
	do {
		__set_current_state(TASK_INTERRUPTIBLE);

		spin_lock_irq(&demo->irq_lock);
		data = demo->irq_data;
		demo->irq_data = 0;
		spin_unlock_irq(&demo->irq_lock);

		if (data != 0) {
			ret = 0;
			break;
		}
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			break;
		}
		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}
		schedule();
	} while (1);
	set_current_state(TASK_RUNNING);
	remove_wait_queue(&demo->irq_queue, &wait);

	/* ��domo�������ж�ȡ���ݲ����䵽Ӧ�ò� */
	if (ret == 0) {
		if (demo->ops->read_callback)
			data = demo->ops->read_callback(demo->dev.parent,
						       data);

		if (sizeof(int) != sizeof(long) &&
		    count == sizeof(unsigned int))
			ret = put_user(data, (unsigned int __user *)buf) ?:
				sizeof(unsigned int);
		else
			ret = put_user(data, (unsigned long __user *)buf) ?:
				sizeof(unsigned long);
	}
	return ret;
}

static unsigned int demo_dev_poll(struct file *file, poll_table *wait)
{
	struct demo_device *demo = file->private_data;
	unsigned long data;

	/* ����ȴ����� */
	poll_wait(file, &demo->irq_queue, wait);

	/* ��ȡ���ݲ��ж������Ƿ�����(�������㱾���ý��̻�˯��) */
	data = demo->irq_data;

	return (data != 0) ? (POLLIN | POLLRDNORM) : 0;
}

static long demo_dev_ioctl(struct file *file,
		unsigned int cmd, unsigned long arg)
{
	struct demo_device *demo = file->private_data;
	struct demo_ctl_data demo_ctl;
	const struct demo_class_ops *ops = demo->ops;
	void __user *uarg = (void __user *) arg;
	int err = 0;

	err = mutex_lock_interruptible(&demo->ops_lock);
	if (err)
		return err;

	switch (cmd) {

	case DEMO_IOCTL_SET:
		/* ����Ȩ������(��ѡ), ���capability.h */
		if (!capable(CAP_SYS_RESOURCE)) {
			err = -EACCES;
			goto done;
		}

		mutex_unlock(&demo->ops_lock);

		if (copy_from_user(&demo_ctl, uarg, sizeof(demo_ctl)))
			return -EFAULT;

		/* demo ʾ����������� */
		return demo_test_set(demo, &demo_ctl);
		
	case DEMO_IOCTL_GET:
		mutex_unlock(&demo->ops_lock);

		/* demo ʾ����ȡ����� */
		err = demo_test_get(demo, &demo_ctl);
		if (err < 0)
			return err;

		if (copy_to_user(uarg, &demo_ctl, sizeof(demo_ctl))) 
			return -EFAULT;

		return err;
		
	default:
		/* ����ʹ����������� ioctl �ӿ� */
		if (ops->ioctl) {
			err = ops->ioctl(demo->dev.parent, cmd, arg);
			if (err == -ENOIOCTLCMD)
				err = -ENOTTY;
		} else
			err = -ENOTTY;
		break;
	}

done:
	mutex_unlock(&demo->ops_lock);
	return err;
}

static int demo_dev_fasync(int fd, struct file *file, int on)
{
	struct demo_device *demo = file->private_data;
	return fasync_helper(fd, file, on, &demo->async_queue);
}

static int demo_dev_release(struct inode *inode, struct file *file)
{
	struct demo_device *demo = file->private_data;

	/* do something while exit */

	/* ���������� release ʵ�� */
	if (demo->ops->release)
		demo->ops->release(demo->dev.parent);

	clear_bit_unlock(DEMO_DEV_BUSY, &demo->flags);
	return 0;
}


static const struct file_operations demo_dev_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.read		= demo_dev_read,
	.poll		= demo_dev_poll,
	.unlocked_ioctl	= demo_dev_ioctl,
	.open		= demo_dev_open,
	.release	= demo_dev_release,
	.fasync		= demo_dev_fasync,
};


void demo_dev_prepare(struct demo_device *demo)
{
	if (!demo_devt)
		return;

	if (demo->id >= DEMO_DEV_MAX) {
		dev_warn(&demo->dev, "%s: too many demo devices\n", demo->name);
		return;
	}

	/* �ַ��豸�ṹ��ʼ�� */
	demo->dev.devt = MKDEV(MAJOR(demo_devt), demo->id);

	cdev_init(&demo->char_dev, &demo_dev_fops);
	demo->char_dev.owner = demo->owner;
}


void demo_dev_add_device(struct demo_device *demo)
{
	/* ע���ַ��豸 */
	if (cdev_add(&demo->char_dev, demo->dev.devt, 1))
		dev_warn(&demo->dev, "%s: failed to add char device %d:%d\n",
			demo->name, MAJOR(demo_devt), demo->id);
	else
		dev_dbg(&demo->dev, "%s: dev (%d:%d)\n", demo->name,
			MAJOR(demo_devt), demo->id);
}

void demo_dev_del_device(struct demo_device *demo)
{
	if (demo->dev.devt)
		cdev_del(&demo->char_dev);
}


void __init demo_dev_init(void)
{
	int err;

	err = alloc_chrdev_region(&demo_devt, 0, DEMO_DEV_MAX, "demo");
	if (err < 0)
		pr_err("failed to allocate char dev region\n");
}

void __exit demo_dev_exit(void)
{
	if (demo_devt)
		unregister_chrdev_region(demo_devt, DEMO_DEV_MAX);
}



